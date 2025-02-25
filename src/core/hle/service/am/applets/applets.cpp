// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "common/assert.h"
#include "core/core.h"
#include "core/frontend/applets/error.h"
#include "core/frontend/applets/general_frontend.h"
#include "core/frontend/applets/profile_select.h"
#include "core/frontend/applets/software_keyboard.h"
#include "core/frontend/applets/web_browser.h"
#include "core/hle/kernel/readable_event.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/writable_event.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applets/applets.h"
#include "core/hle/service/am/applets/error.h"
#include "core/hle/service/am/applets/general_backend.h"
#include "core/hle/service/am/applets/profile_select.h"
#include "core/hle/service/am/applets/software_keyboard.h"
#include "core/hle/service/am/applets/web_browser.h"

namespace Service::AM::Applets {

AppletDataBroker::AppletDataBroker() {
    auto& kernel = Core::System::GetInstance().Kernel();
    state_changed_event = Kernel::WritableEvent::CreateEventPair(
        kernel, Kernel::ResetType::Manual, "ILibraryAppletAccessor:StateChangedEvent");
    pop_out_data_event = Kernel::WritableEvent::CreateEventPair(
        kernel, Kernel::ResetType::Manual, "ILibraryAppletAccessor:PopDataOutEvent");
    pop_interactive_out_data_event = Kernel::WritableEvent::CreateEventPair(
        kernel, Kernel::ResetType::Manual, "ILibraryAppletAccessor:PopInteractiveDataOutEvent");
}

AppletDataBroker::~AppletDataBroker() = default;

std::unique_ptr<IStorage> AppletDataBroker::PopNormalDataToGame() {
    if (out_channel.empty())
        return nullptr;

    auto out = std::move(out_channel.front());
    out_channel.pop();
    return out;
}

std::unique_ptr<IStorage> AppletDataBroker::PopNormalDataToApplet() {
    if (in_channel.empty())
        return nullptr;

    auto out = std::move(in_channel.front());
    in_channel.pop();
    return out;
}

std::unique_ptr<IStorage> AppletDataBroker::PopInteractiveDataToGame() {
    if (out_interactive_channel.empty())
        return nullptr;

    auto out = std::move(out_interactive_channel.front());
    out_interactive_channel.pop();
    return out;
}

std::unique_ptr<IStorage> AppletDataBroker::PopInteractiveDataToApplet() {
    if (in_interactive_channel.empty())
        return nullptr;

    auto out = std::move(in_interactive_channel.front());
    in_interactive_channel.pop();
    return out;
}

void AppletDataBroker::PushNormalDataFromGame(IStorage storage) {
    in_channel.push(std::make_unique<IStorage>(storage));
}

void AppletDataBroker::PushNormalDataFromApplet(IStorage storage) {
    out_channel.push(std::make_unique<IStorage>(storage));
    pop_out_data_event.writable->Signal();
}

void AppletDataBroker::PushInteractiveDataFromGame(IStorage storage) {
    in_interactive_channel.push(std::make_unique<IStorage>(storage));
}

void AppletDataBroker::PushInteractiveDataFromApplet(IStorage storage) {
    out_interactive_channel.push(std::make_unique<IStorage>(storage));
    pop_interactive_out_data_event.writable->Signal();
}

void AppletDataBroker::SignalStateChanged() const {
    state_changed_event.writable->Signal();
}

Kernel::SharedPtr<Kernel::ReadableEvent> AppletDataBroker::GetNormalDataEvent() const {
    return pop_out_data_event.readable;
}

Kernel::SharedPtr<Kernel::ReadableEvent> AppletDataBroker::GetInteractiveDataEvent() const {
    return pop_interactive_out_data_event.readable;
}

Kernel::SharedPtr<Kernel::ReadableEvent> AppletDataBroker::GetStateChangedEvent() const {
    return state_changed_event.readable;
}

Applet::Applet() = default;

Applet::~Applet() = default;

void Applet::Initialize() {
    const auto common = broker.PopNormalDataToApplet();
    ASSERT(common != nullptr);

    const auto common_data = common->GetData();

    ASSERT(common_data.size() >= sizeof(CommonArguments));
    std::memcpy(&common_args, common_data.data(), sizeof(CommonArguments));

    initialized = true;
}

AppletFrontendSet::AppletFrontendSet() = default;

AppletFrontendSet::AppletFrontendSet(ErrorApplet error, PhotoViewer photo_viewer,
                                     ProfileSelect profile_select,
                                     SoftwareKeyboard software_keyboard, WebBrowser web_browser)
    : error{std::move(error)}, photo_viewer{std::move(photo_viewer)}, profile_select{std::move(
                                                                          profile_select)},
      software_keyboard{std::move(software_keyboard)}, web_browser{std::move(web_browser)} {}

AppletFrontendSet::~AppletFrontendSet() = default;

AppletFrontendSet::AppletFrontendSet(AppletFrontendSet&&) noexcept = default;

AppletFrontendSet& AppletFrontendSet::operator=(AppletFrontendSet&&) noexcept = default;

AppletManager::AppletManager() = default;

AppletManager::~AppletManager() = default;

const AppletFrontendSet& AppletManager::GetAppletFrontendSet() const {
    return frontend;
}

void AppletManager::SetAppletFrontendSet(AppletFrontendSet set) {
    if (set.error != nullptr)
        frontend.error = std::move(set.error);
    if (set.photo_viewer != nullptr)
        frontend.photo_viewer = std::move(set.photo_viewer);
    if (set.profile_select != nullptr)
        frontend.profile_select = std::move(set.profile_select);
    if (set.software_keyboard != nullptr)
        frontend.software_keyboard = std::move(set.software_keyboard);
    if (set.web_browser != nullptr)
        frontend.web_browser = std::move(set.web_browser);
}

void AppletManager::SetDefaultAppletFrontendSet() {
    frontend.error = std::make_unique<Core::Frontend::DefaultErrorApplet>();
    frontend.photo_viewer = std::make_unique<Core::Frontend::DefaultPhotoViewerApplet>();
    frontend.profile_select = std::make_unique<Core::Frontend::DefaultProfileSelectApplet>();
    frontend.software_keyboard = std::make_unique<Core::Frontend::DefaultSoftwareKeyboardApplet>();
    frontend.web_browser = std::make_unique<Core::Frontend::DefaultWebBrowserApplet>();
}

void AppletManager::SetDefaultAppletsIfMissing() {
    if (frontend.error == nullptr) {
        frontend.error = std::make_unique<Core::Frontend::DefaultErrorApplet>();
    }

    if (frontend.photo_viewer == nullptr) {
        frontend.photo_viewer = std::make_unique<Core::Frontend::DefaultPhotoViewerApplet>();
    }

    if (frontend.profile_select == nullptr) {
        frontend.profile_select = std::make_unique<Core::Frontend::DefaultProfileSelectApplet>();
    }

    if (frontend.software_keyboard == nullptr) {
        frontend.software_keyboard =
            std::make_unique<Core::Frontend::DefaultSoftwareKeyboardApplet>();
    }

    if (frontend.web_browser == nullptr) {
        frontend.web_browser = std::make_unique<Core::Frontend::DefaultWebBrowserApplet>();
    }
}

void AppletManager::ClearAll() {
    frontend = {};
}

std::shared_ptr<Applet> AppletManager::GetApplet(AppletId id) const {
    switch (id) {
    case AppletId::Error:
        return std::make_shared<Error>(*frontend.error);
    case AppletId::ProfileSelect:
        return std::make_shared<ProfileSelect>(*frontend.profile_select);
    case AppletId::SoftwareKeyboard:
        return std::make_shared<SoftwareKeyboard>(*frontend.software_keyboard);
    case AppletId::PhotoViewer:
        return std::make_shared<PhotoViewer>(*frontend.photo_viewer);
    case AppletId::LibAppletOff:
        return std::make_shared<WebBrowser>(*frontend.web_browser);
    default:
        UNIMPLEMENTED_MSG(
            "No backend implementation exists for applet_id={:02X}! Falling back to stub applet.",
            static_cast<u8>(id));
        return std::make_shared<StubApplet>();
    }
}

} // namespace Service::AM::Applets
