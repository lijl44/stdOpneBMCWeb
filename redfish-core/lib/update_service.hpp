/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "dbus_utility.hpp"
#include "multipart_parser.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "task.hpp"
#include "utils/collection.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/sw_utils.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/format.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/unpack_properties.hpp>

#include <array>
#include <filesystem>
#include <string_view>

namespace redfish
{

// Match signals added on software path
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<sdbusplus::bus::match_t> fwUpdateMatcher;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<sdbusplus::bus::match_t> fwUpdateErrorMatcher;
// Only allow one update at a time
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static bool fwUpdateInProgress = false;
// Timer for software available
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<boost::asio::steady_timer> fwAvailableTimer;

inline static void cleanUp()
{
    fwUpdateInProgress = false;
    fwUpdateMatcher = nullptr;
    fwUpdateErrorMatcher = nullptr;
}
inline static void activateImage(const std::string& objPath,
                                 const std::string& service)
{
    BMCWEB_LOG_DEBUG << "Activate image for " << objPath << " " << service;
    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Software.Activation", "RequestedActivation",
        "xyz.openbmc_project.Software.Activation.RequestedActivations.Active",
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "error_code = " << ec;
            BMCWEB_LOG_DEBUG << "error msg = " << ec.message();
        }
        });
}

// Note that asyncResp can be either a valid pointer or nullptr. If nullptr
// then no asyncResp updates will occur
static void
    softwareInterfaceAdded(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           sdbusplus::message_t& m, task::Payload&& payload)
{
    dbus::utility::DBusInteracesMap interfacesProperties;

    sdbusplus::message::object_path objPath;

    m.read(objPath, interfacesProperties);

    BMCWEB_LOG_DEBUG << "obj path = " << objPath.str;
    for (const auto& interface : interfacesProperties)
    {
        BMCWEB_LOG_DEBUG << "interface = " << interface.first;

        if (interface.first == "xyz.openbmc_project.Software.Activation")
        {
            // Retrieve service and activate
            constexpr std::array<std::string_view, 1> interfaces = {
                "xyz.openbmc_project.Software.Activation"};
            dbus::utility::getDbusObject(
                objPath.str, interfaces,
                [objPath, asyncResp, payload(std::move(payload))](
                    const boost::system::error_code& ec,
                    const std::vector<
                        std::pair<std::string, std::vector<std::string>>>&
                        objInfo) mutable {
                if (ec)
                {
                    BMCWEB_LOG_DEBUG << "error_code = " << ec;
                    BMCWEB_LOG_DEBUG << "error msg = " << ec.message();
                    if (asyncResp)
                    {
                        messages::internalError(asyncResp->res);
                    }
                    cleanUp();
                    return;
                }
                // Ensure we only got one service back
                if (objInfo.size() != 1)
                {
                    BMCWEB_LOG_ERROR << "Invalid Object Size "
                                     << objInfo.size();
                    if (asyncResp)
                    {
                        messages::internalError(asyncResp->res);
                    }
                    cleanUp();
                    return;
                }
                // cancel timer only when
                // xyz.openbmc_project.Software.Activation interface
                // is added
                fwAvailableTimer = nullptr;

                activateImage(objPath.str, objInfo[0].first);
                if (asyncResp)
                {
                    std::shared_ptr<task::TaskData> task =
                        task::TaskData::createTask(
                            [](const boost::system::error_code& ec2,
                               sdbusplus::message_t& msg,
                               const std::shared_ptr<task::TaskData>&
                                   taskData) {
                        if (ec2)
                        {
                            return task::completed;
                        }

                        std::string iface;
                        dbus::utility::DBusPropertiesMap values;

                        std::string index = std::to_string(taskData->index);
                        msg.read(iface, values);

                        if (iface == "xyz.openbmc_project.Software.Activation")
                        {
                            const std::string* state = nullptr;
                            for (const auto& property : values)
                            {
                                if (property.first == "Activation")
                                {
                                    state = std::get_if<std::string>(
                                        &property.second);
                                    if (state == nullptr)
                                    {
                                        taskData->messages.emplace_back(
                                            messages::internalError());
                                        return task::completed;
                                    }
                                }
                            }

                            if (state == nullptr)
                            {
                                return !task::completed;
                            }

                            if (state->ends_with("Invalid") ||
                                state->ends_with("Failed"))
                            {
                                taskData->state = "Exception";
                                taskData->status = "Warning";
                                taskData->messages.emplace_back(
                                    messages::taskAborted(index));
                                return task::completed;
                            }

                            if (state->ends_with("Staged"))
                            {
                                taskData->state = "Stopping";
                                taskData->messages.emplace_back(
                                    messages::taskPaused(index));

                                // its staged, set a long timer to
                                // allow them time to complete the
                                // update (probably cycle the
                                // system) if this expires then
                                // task will be cancelled
                                taskData->extendTimer(std::chrono::hours(5));
                                return !task::completed;
                            }

                            if (state->ends_with("Active"))
                            {
                                taskData->messages.emplace_back(
                                    messages::taskCompletedOK(index));
                                taskData->state = "Completed";
                                return task::completed;
                            }
                        }
                        else if (
                            iface ==
                            "xyz.openbmc_project.Software.ActivationProgress")
                        {
                            const uint8_t* progress = nullptr;
                            for (const auto& property : values)
                            {
                                if (property.first == "Progress")
                                {
                                    progress =
                                        std::get_if<uint8_t>(&property.second);
                                    if (progress == nullptr)
                                    {
                                        taskData->messages.emplace_back(
                                            messages::internalError());
                                        return task::completed;
                                    }
                                }
                            }

                            if (progress == nullptr)
                            {
                                return !task::completed;
                            }
                            taskData->percentComplete = *progress;
                            taskData->messages.emplace_back(
                                messages::taskProgressChanged(index,
                                                              *progress));

                            // if we're getting status updates it's
                            // still alive, update timer
                            taskData->extendTimer(std::chrono::minutes(5));
                        }

                        // as firmware update often results in a
                        // reboot, the task  may never "complete"
                        // unless it is an error

                        return !task::completed;
                            },
                            "type='signal',interface='org.freedesktop.DBus.Properties',"
                            "member='PropertiesChanged',path='" +
                                objPath.str + "'");
                    task->startTimer(std::chrono::minutes(5));
                    task->populateResp(asyncResp->res);
                    task->payload.emplace(std::move(payload));
                }
                fwUpdateInProgress = false;
                });

            break;
        }
    }
}

// Note that asyncResp can be either a valid pointer or nullptr. If nullptr
// then no asyncResp updates will occur
static void monitorForSoftwareAvailable(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const crow::Request& req, const std::string& url,
    int timeoutTimeSeconds = 25)
{
    // Only allow one FW update at a time
    if (fwUpdateInProgress)
    {
        if (asyncResp)
        {
            messages::serviceTemporarilyUnavailable(asyncResp->res, "30");
        }
        return;
    }

    fwAvailableTimer =
        std::make_unique<boost::asio::steady_timer>(*req.ioService);

    fwAvailableTimer->expires_after(std::chrono::seconds(timeoutTimeSeconds));

    fwAvailableTimer->async_wait(
        [asyncResp](const boost::system::error_code& ec) {
        cleanUp();
        if (ec == boost::asio::error::operation_aborted)
        {
            // expected, we were canceled before the timer completed.
            return;
        }
        BMCWEB_LOG_ERROR
            << "Timed out waiting for firmware object being created";
        BMCWEB_LOG_ERROR << "FW image may has already been uploaded to server";
        if (ec)
        {
            BMCWEB_LOG_ERROR << "Async_wait failed" << ec;
            return;
        }
        if (asyncResp)
        {
            redfish::messages::internalError(asyncResp->res);
        }
    });
    task::Payload payload(req);
    auto callback = [asyncResp, payload](sdbusplus::message_t& m) mutable {
        BMCWEB_LOG_DEBUG << "Match fired";
        softwareInterfaceAdded(asyncResp, m, std::move(payload));
    };

    fwUpdateInProgress = true;

    fwUpdateMatcher = std::make_unique<sdbusplus::bus::match_t>(
        *crow::connections::systemBus,
        "interface='org.freedesktop.DBus.ObjectManager',type='signal',"
        "member='InterfacesAdded',path='/xyz/openbmc_project/software'",
        callback);

    fwUpdateErrorMatcher = std::make_unique<sdbusplus::bus::match_t>(
        *crow::connections::systemBus,
        "interface='org.freedesktop.DBus.ObjectManager',type='signal',"
        "member='InterfacesAdded',"
        "path='/xyz/openbmc_project/logging'",
        [asyncResp, url](sdbusplus::message_t& m) {
        std::vector<std::pair<std::string, dbus::utility::DBusPropertiesMap>>
            interfacesProperties;
        sdbusplus::message::object_path objPath;
        m.read(objPath, interfacesProperties);
        BMCWEB_LOG_DEBUG << "obj path = " << objPath.str;
        for (const std::pair<std::string, dbus::utility::DBusPropertiesMap>&
                 interface : interfacesProperties)
        {
            if (interface.first == "xyz.openbmc_project.Logging.Entry")
            {
                for (const std::pair<std::string,
                                     dbus::utility::DbusVariantType>& value :
                     interface.second)
                {
                    if (value.first != "Message")
                    {
                        continue;
                    }
                    const std::string* type =
                        std::get_if<std::string>(&value.second);
                    if (type == nullptr)
                    {
                        // if this was our message, timeout will cover it
                        return;
                    }
                    fwAvailableTimer = nullptr;
                    if (*type ==
                        "xyz.openbmc_project.Software.Image.Error.UnTarFailure")
                    {
                        redfish::messages::invalidUpload(asyncResp->res, url,
                                                         "Invalid archive");
                    }
                    else if (*type ==
                             "xyz.openbmc_project.Software.Image.Error."
                             "ManifestFileFailure")
                    {
                        redfish::messages::invalidUpload(asyncResp->res, url,
                                                         "Invalid manifest");
                    }
                    else if (
                        *type ==
                        "xyz.openbmc_project.Software.Image.Error.ImageFailure")
                    {
                        redfish::messages::invalidUpload(
                            asyncResp->res, url, "Invalid image format");
                    }
                    else if (
                        *type ==
                        "xyz.openbmc_project.Software.Version.Error.AlreadyExists")
                    {
                        redfish::messages::invalidUpload(
                            asyncResp->res, url,
                            "Image version already exists");

                        redfish::messages::resourceAlreadyExists(
                            asyncResp->res, "UpdateService", "Version",
                            "uploaded version");
                    }
                    else if (
                        *type ==
                        "xyz.openbmc_project.Software.Image.Error.BusyFailure")
                    {
                        redfish::messages::resourceExhaustion(asyncResp->res,
                                                              url);
                    }
                    else
                    {
                        redfish::messages::internalError(asyncResp->res);
                    }
                }
            }
        }
        });
}

/**
 * UpdateServiceActionsSimpleUpdate class supports handle POST method for
 * SimpleUpdate action.
 */
inline void requestRoutesUpdateServiceActionsSimpleUpdate(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate/")
        .privileges(redfish::privileges::postUpdateService)
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        std::optional<std::string> transferProtocol;
        std::string imageURI;

        BMCWEB_LOG_DEBUG << "Enter UpdateService.SimpleUpdate doPost";

        // User can pass in both TransferProtocol and ImageURI parameters or
        // they can pass in just the ImageURI with the transfer protocol
        // embedded within it.
        // 1) TransferProtocol:TFTP ImageURI:1.1.1.1/myfile.bin
        // 2) ImageURI:tftp://1.1.1.1/myfile.bin

        if (!json_util::readJsonAction(req, asyncResp->res, "TransferProtocol",
                                       transferProtocol, "ImageURI", imageURI))
        {
            BMCWEB_LOG_DEBUG
                << "Missing TransferProtocol or ImageURI parameter";
            return;
        }
        if (!transferProtocol)
        {
            // Must be option 2
            // Verify ImageURI has transfer protocol in it
            size_t separator = imageURI.find(':');
            if ((separator == std::string::npos) ||
                ((separator + 1) > imageURI.size()))
            {
                messages::actionParameterValueTypeError(
                    asyncResp->res, imageURI, "ImageURI",
                    "UpdateService.SimpleUpdate");
                BMCWEB_LOG_ERROR << "ImageURI missing transfer protocol: "
                                 << imageURI;
                return;
            }
            transferProtocol = imageURI.substr(0, separator);
            // Ensure protocol is upper case for a common comparison path
            // below
            boost::to_upper(*transferProtocol);
            BMCWEB_LOG_DEBUG << "Encoded transfer protocol "
                             << *transferProtocol;

            // Adjust imageURI to not have the protocol on it for parsing
            // below
            // ex. tftp://1.1.1.1/myfile.bin -> 1.1.1.1/myfile.bin
            imageURI = imageURI.substr(separator + 3);
            BMCWEB_LOG_DEBUG << "Adjusted imageUri " << imageURI;
        }

        // OpenBMC currently only supports TFTP
        if (*transferProtocol != "TFTP")
        {
            messages::actionParameterNotSupported(asyncResp->res,
                                                  "TransferProtocol",
                                                  "UpdateService.SimpleUpdate");
            BMCWEB_LOG_ERROR << "Request incorrect protocol parameter: "
                             << *transferProtocol;
            return;
        }

        // Format should be <IP or Hostname>/<file> for imageURI
        size_t separator = imageURI.find('/');
        if ((separator == std::string::npos) ||
            ((separator + 1) > imageURI.size()))
        {
            messages::actionParameterValueTypeError(
                asyncResp->res, imageURI, "ImageURI",
                "UpdateService.SimpleUpdate");
            BMCWEB_LOG_ERROR << "Invalid ImageURI: " << imageURI;
            return;
        }

        std::string tftpServer = imageURI.substr(0, separator);
        std::string fwFile = imageURI.substr(separator + 1);
        BMCWEB_LOG_DEBUG << "Server: " << tftpServer + " File: " << fwFile;

        // Setup callback for when new software detected
        // Give TFTP 10 minutes to complete
        monitorForSoftwareAvailable(
            asyncResp, req,
            "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate",
            600);

        // TFTP can take up to 10 minutes depending on image size and
        // connection speed. Return to caller as soon as the TFTP operation
        // has been started. The callback above will ensure the activate
        // is started once the download has completed
        redfish::messages::success(asyncResp->res);

        // Call TFTP service
        crow::connections::systemBus->async_method_call(
            [](const boost::system::error_code& ec) {
            if (ec)
            {
                // messages::internalError(asyncResp->res);
                cleanUp();
                BMCWEB_LOG_DEBUG << "error_code = " << ec;
                BMCWEB_LOG_DEBUG << "error msg = " << ec.message();
            }
            else
            {
                BMCWEB_LOG_DEBUG << "Call to DownloaViaTFTP Success";
            }
            },
            "xyz.openbmc_project.Software.Download",
            "/xyz/openbmc_project/software", "xyz.openbmc_project.Common.TFTP",
            "DownloadViaTFTP", fwFile, tftpServer);

        BMCWEB_LOG_DEBUG << "Exit UpdateService.SimpleUpdate doPost";
        });
}

inline void uploadImageFile(crow::Response& res, std::string_view body)
{
    std::filesystem::path filepath(
        "/tmp/images/" +
        boost::uuids::to_string(boost::uuids::random_generator()()));
    BMCWEB_LOG_DEBUG << "Writing file to " << filepath;
    std::ofstream out(filepath, std::ofstream::out | std::ofstream::binary |
                                    std::ofstream::trunc);
    // set the permission of the file to 640
    std::filesystem::perms permission = std::filesystem::perms::owner_read |
                                        std::filesystem::perms::group_read;
    std::filesystem::permissions(filepath, permission);
    out << body;

    if (out.bad())
    {
        messages::internalError(res);
        cleanUp();
    }
}

inline void setApplyTime(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const std::string& applyTime)
{
    std::string applyTimeNewVal;
    if (applyTime == "Immediate")
    {
        applyTimeNewVal =
            "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate";
    }
    else if (applyTime == "OnReset")
    {
        applyTimeNewVal =
            "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.OnReset";
    }
    else
    {
        BMCWEB_LOG_INFO
            << "ApplyTime value is not in the list of acceptable values";
        messages::propertyValueNotInList(asyncResp->res, applyTime,
                                         "ApplyTime");
        return;
    }

    // Set the requested image apply time value
    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, "xyz.openbmc_project.Settings",
        "/xyz/openbmc_project/software/apply_time",
        "xyz.openbmc_project.Software.ApplyTime", "RequestedApplyTime",
        applyTimeNewVal, [asyncResp](const boost::system::error_code& ec) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "D-Bus responses error: " << ec;
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
        });
}

inline void
    updateMultipartContext(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const MultipartParser& parser)
{
    const std::string* uploadData = nullptr;
    std::optional<std::string> applyTime = "OnReset";
    bool targetFound = false;
    for (const FormPart& formpart : parser.mime_fields)
    {
        boost::beast::http::fields::const_iterator it =
            formpart.fields.find("Content-Disposition");
        if (it == formpart.fields.end())
        {
            BMCWEB_LOG_ERROR << "Couldn't find Content-Disposition";
            return;
        }
        BMCWEB_LOG_INFO << "Parsing value " << it->value();

        // The construction parameters of param_list must start with `;`
        size_t index = it->value().find(';');
        if (index == std::string::npos)
        {
            continue;
        }

        for (const auto& param :
             boost::beast::http::param_list{it->value().substr(index)})
        {
            if (param.first != "name" || param.second.empty())
            {
                continue;
            }

            if (param.second == "UpdateParameters")
            {
                std::vector<std::string> targets;
                nlohmann::json content =
                    nlohmann::json::parse(formpart.content);
                if (!json_util::readJson(content, asyncResp->res, "Targets",
                                         targets, "@Redfish.OperationApplyTime",
                                         applyTime))
                {
                    return;
                }
                if (targets.size() != 1)
                {
                    messages::propertyValueFormatError(asyncResp->res,
                                                       "Targets", "");
                    return;
                }
                if (targets[0] != "/redfish/v1/Managers/bmc")
                {
                    messages::propertyValueNotInList(asyncResp->res,
                                                     "Targets/0", targets[0]);
                    return;
                }
                targetFound = true;
            }
            else if (param.second == "UpdateFile")
            {
                uploadData = &(formpart.content);
            }
        }
    }

    if (uploadData == nullptr)
    {
        BMCWEB_LOG_ERROR << "Upload data is NULL";
        messages::propertyMissing(asyncResp->res, "UpdateFile");
        return;
    }
    if (!targetFound)
    {
        messages::propertyMissing(asyncResp->res, "targets");
        return;
    }

    setApplyTime(asyncResp, *applyTime);

    uploadImageFile(asyncResp->res, *uploadData);
}

inline void
    handleUpdateServicePost(App& app, const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    std::string_view contentType = req.getHeaderValue("Content-Type");

    BMCWEB_LOG_DEBUG << "doPost: contentType=" << contentType;

    // Make sure that content type is application/octet-stream or
    // multipart/form-data
    if (boost::iequals(contentType, "application/octet-stream"))
    {
        // Setup callback for when new software detected
        monitorForSoftwareAvailable(asyncResp, req,
                                    "/redfish/v1/UpdateService");

        uploadImageFile(asyncResp->res, req.body());
    }
    else if (contentType.starts_with("multipart/form-data"))
    {
        MultipartParser parser;

        // Setup callback for when new software detected
        monitorForSoftwareAvailable(asyncResp, req,
                                    "/redfish/v1/UpdateService");

        ParserError ec = parser.parse(req);
        if (ec != ParserError::PARSER_SUCCESS)
        {
            // handle error
            BMCWEB_LOG_ERROR << "MIME parse failed, ec : "
                             << static_cast<int>(ec);
            messages::internalError(asyncResp->res);
            return;
        }
        updateMultipartContext(asyncResp, parser);
    }
    else
    {
        BMCWEB_LOG_DEBUG << "Bad content type specified:" << contentType;
        asyncResp->res.result(boost::beast::http::status::bad_request);
    }
}

inline void requestRoutesUpdateService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/")
        .privileges(redfish::privileges::getUpdateService)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        asyncResp->res.jsonValue["@odata.type"] =
            "#UpdateService.v1_11_1.UpdateService";
        asyncResp->res.jsonValue["@odata.id"] = "/redfish/v1/UpdateService";
        asyncResp->res.jsonValue["Id"] = "UpdateService";
        asyncResp->res.jsonValue["Description"] = "Service for Software Update";
        asyncResp->res.jsonValue["Name"] = "Update Service";

        asyncResp->res.jsonValue["HttpPushUri"] =
            "/redfish/v1/UpdateService/update";
        asyncResp->res.jsonValue["MultipartHttpPushUri"] =
            "/redfish/v1/UpdateService/update";

        // UpdateService cannot be disabled
        asyncResp->res.jsonValue["ServiceEnabled"] = true;
        asyncResp->res.jsonValue["FirmwareInventory"]["@odata.id"] =
            "/redfish/v1/UpdateService/FirmwareInventory";
        // Get the MaxImageSizeBytes
        asyncResp->res.jsonValue["MaxImageSizeBytes"] =
            bmcwebHttpReqBodyLimitMb * 1024 * 1024;

#ifdef BMCWEB_INSECURE_ENABLE_REDFISH_FW_TFTP_UPDATE
        // Update Actions object.
        nlohmann::json& updateSvcSimpleUpdate =
            asyncResp->res.jsonValue["Actions"]["#UpdateService.SimpleUpdate"];
        updateSvcSimpleUpdate["target"] =
            "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate";
        updateSvcSimpleUpdate["TransferProtocol@Redfish.AllowableValues"] = {
            "TFTP"};
#endif
        // Get the current ApplyTime value
        sdbusplus::asio::getProperty<std::string>(
            *crow::connections::systemBus, "xyz.openbmc_project.Settings",
            "/xyz/openbmc_project/software/apply_time",
            "xyz.openbmc_project.Software.ApplyTime", "RequestedApplyTime",
            [asyncResp](const boost::system::error_code& ec,
                        const std::string& applyTime) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            // Store the ApplyTime Value
            if (applyTime == "xyz.openbmc_project.Software.ApplyTime."
                             "RequestedApplyTimes.Immediate")
            {
                asyncResp->res.jsonValue["HttpPushUriOptions"]
                                        ["HttpPushUriApplyTime"]["ApplyTime"] =
                    "Immediate";
            }
            else if (applyTime == "xyz.openbmc_project.Software.ApplyTime."
                                  "RequestedApplyTimes.OnReset")
            {
                asyncResp->res.jsonValue["HttpPushUriOptions"]
                                        ["HttpPushUriApplyTime"]["ApplyTime"] =
                    "OnReset";
            }
            });
        });
    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/")
        .privileges(redfish::privileges::patchUpdateService)
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        BMCWEB_LOG_DEBUG << "doPatch...";

        std::optional<nlohmann::json> pushUriOptions;
        if (!json_util::readJsonPatch(req, asyncResp->res, "HttpPushUriOptions",
                                      pushUriOptions))
        {
            return;
        }

        if (pushUriOptions)
        {
            std::optional<nlohmann::json> pushUriApplyTime;
            if (!json_util::readJson(*pushUriOptions, asyncResp->res,
                                     "HttpPushUriApplyTime", pushUriApplyTime))
            {
                return;
            }

            if (pushUriApplyTime)
            {
                std::optional<std::string> applyTime;
                if (!json_util::readJson(*pushUriApplyTime, asyncResp->res,
                                         "ApplyTime", applyTime))
                {
                    return;
                }

                if (applyTime)
                {
                    setApplyTime(asyncResp, *applyTime);
                }
            }
        }
        });

    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/update/")
        .privileges(redfish::privileges::postUpdateService)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleUpdateServicePost, std::ref(app)));
}

inline void requestRoutesSoftwareInventoryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/FirmwareInventory/")
        .privileges(redfish::privileges::getSoftwareInventoryCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        asyncResp->res.jsonValue["@odata.type"] =
            "#SoftwareInventoryCollection.SoftwareInventoryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/UpdateService/FirmwareInventory";
        asyncResp->res.jsonValue["Name"] = "Software Inventory Collection";
        const std::array<const std::string_view, 1> iface = {
            "xyz.openbmc_project.Software.Version"};

        redfish::collection_util::getCollectionMembers(
            asyncResp,
            boost::urls::url("/redfish/v1/UpdateService/FirmwareInventory"),
            iface, "/xyz/openbmc_project/software");
        });
}
/* Fill related item links (i.e. bmc, bios) in for inventory */
inline static void
    getRelatedItems(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                    const std::string& purpose)
{
    if (purpose == sw_util::bmcPurpose)
    {
        nlohmann::json& relatedItem = asyncResp->res.jsonValue["RelatedItem"];
        nlohmann::json::object_t item;
        item["@odata.id"] = "/redfish/v1/Managers/bmc";
        relatedItem.emplace_back(std::move(item));
        asyncResp->res.jsonValue["RelatedItem@odata.count"] =
            relatedItem.size();
    }
    else if (purpose == sw_util::biosPurpose)
    {
        nlohmann::json& relatedItem = asyncResp->res.jsonValue["RelatedItem"];
        nlohmann::json::object_t item;
        item["@odata.id"] = "/redfish/v1/Systems/system/Bios";
        relatedItem.emplace_back(std::move(item));
        asyncResp->res.jsonValue["RelatedItem@odata.count"] =
            relatedItem.size();
    }
    else
    {
        BMCWEB_LOG_ERROR << "Unknown software purpose " << purpose;
    }
}

inline void
    getSoftwareVersion(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& service, const std::string& path,
                       const std::string& swId)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, path,
        "xyz.openbmc_project.Software.Version",
        [asyncResp,
         swId](const boost::system::error_code& ec,
               const dbus::utility::DBusPropertiesMap& propertiesList) {
        if (ec)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        const std::string* swInvPurpose = nullptr;
        const std::string* version = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "Purpose",
            swInvPurpose, "Version", version);

        if (!success)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        if (swInvPurpose == nullptr)
        {
            BMCWEB_LOG_DEBUG << "Can't find property \"Purpose\"!";
            messages::internalError(asyncResp->res);
            return;
        }

        BMCWEB_LOG_DEBUG << "swInvPurpose = " << *swInvPurpose;

        if (version == nullptr)
        {
            BMCWEB_LOG_DEBUG << "Can't find property \"Version\"!";

            messages::internalError(asyncResp->res);

            return;
        }
        asyncResp->res.jsonValue["Version"] = *version;
        asyncResp->res.jsonValue["Id"] = swId;

        // swInvPurpose is of format:
        // xyz.openbmc_project.Software.Version.VersionPurpose.ABC
        // Translate this to "ABC image"
        size_t endDesc = swInvPurpose->rfind('.');
        if (endDesc == std::string::npos)
        {
            messages::internalError(asyncResp->res);
            return;
        }
        endDesc++;
        if (endDesc >= swInvPurpose->size())
        {
            messages::internalError(asyncResp->res);
            return;
        }

        std::string formatDesc = swInvPurpose->substr(endDesc);
        asyncResp->res.jsonValue["Description"] = formatDesc + " image";
        getRelatedItems(asyncResp, *swInvPurpose);
        });
}

inline void requestRoutesSoftwareInventory(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/FirmwareInventory/<str>/")
        .privileges(redfish::privileges::getSoftwareInventory)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& param) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        std::shared_ptr<std::string> swId =
            std::make_shared<std::string>(param);

        asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
            "/redfish/v1/UpdateService/FirmwareInventory/{}", *swId);

        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Software.Version"};
        dbus::utility::getSubTree(
            "/", 0, interfaces,
            [asyncResp,
             swId](const boost::system::error_code& ec,
                   const dbus::utility::MapperGetSubTreeResponse& subtree) {
            BMCWEB_LOG_DEBUG << "doGet callback...";
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            // Ensure we find our input swId, otherwise return an error
            bool found = false;
            for (const std::pair<std::string,
                                 std::vector<std::pair<
                                     std::string, std::vector<std::string>>>>&
                     obj : subtree)
            {
                if (!obj.first.ends_with(*swId))
                {
                    continue;
                }

                if (obj.second.empty())
                {
                    continue;
                }

                found = true;
                sw_util::getSwStatus(asyncResp, swId, obj.second[0].first);
                getSoftwareVersion(asyncResp, obj.second[0].first, obj.first,
                                   *swId);
            }
            if (!found)
            {
                BMCWEB_LOG_WARNING << "Input swID " << *swId << " not found!";
                messages::resourceMissingAtURI(
                    asyncResp->res,
                    boost::urls::format(
                        "/redfish/v1/UpdateService/FirmwareInventory/{}",
                        *swId));
                return;
            }
            asyncResp->res.jsonValue["@odata.type"] =
                "#SoftwareInventory.v1_1_0.SoftwareInventory";
            asyncResp->res.jsonValue["Name"] = "Software Inventory";
            asyncResp->res.jsonValue["Status"]["HealthRollup"] = "OK";

            asyncResp->res.jsonValue["Updateable"] = false;
            sw_util::getSwUpdatableStatus(asyncResp, swId);
            });
        });
}

} // namespace redfish
