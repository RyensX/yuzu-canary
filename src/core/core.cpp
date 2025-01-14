// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <memory>
#include <utility>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/core_cpu.h"
#include "core/core_timing.h"
#include "core/cpu_core_manager.h"
#include "core/file_sys/mode.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs_concat.h"
#include "core/file_sys/vfs_real.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/scheduler.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/am/applets/applets.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "core/settings.h"
#include "core/telemetry_session.h"
#include "file_sys/cheat_engine.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

namespace Core {

/*static*/ System System::s_instance;

FileSys::VirtualFile GetGameFileFromPath(const FileSys::VirtualFilesystem& vfs,
                                         const std::string& path) {
    // To account for split 00+01+etc files.
    std::string dir_name;
    std::string filename;
    Common::SplitPath(path, &dir_name, &filename, nullptr);
    if (filename == "00") {
        const auto dir = vfs->OpenDirectory(dir_name, FileSys::Mode::Read);
        std::vector<FileSys::VirtualFile> concat;
        for (u8 i = 0; i < 0x10; ++i) {
            auto next = dir->GetFile(fmt::format("{:02X}", i));
            if (next != nullptr)
                concat.push_back(std::move(next));
            else {
                next = dir->GetFile(fmt::format("{:02x}", i));
                if (next != nullptr)
                    concat.push_back(std::move(next));
                else
                    break;
            }
        }

        if (concat.empty())
            return nullptr;

        return FileSys::ConcatenatedVfsFile::MakeConcatenatedFile(concat, dir->GetName());
    }

    if (FileUtil::IsDirectory(path))
        return vfs->OpenFile(path + "/" + "main", FileSys::Mode::Read);

    return vfs->OpenFile(path, FileSys::Mode::Read);
}
struct System::Impl {
    explicit Impl(System& system) : kernel{system}, cpu_core_manager{system} {}

    Cpu& CurrentCpuCore() {
        return cpu_core_manager.GetCurrentCore();
    }

    ResultStatus RunLoop(bool tight_loop) {
        status = ResultStatus::Success;

        cpu_core_manager.RunLoop(tight_loop);

        return status;
    }

    ResultStatus Init(System& system, Frontend::EmuWindow& emu_window) {
        LOG_DEBUG(HW_Memory, "initialized OK");

        core_timing.Initialize();
        cpu_core_manager.Initialize();
        kernel.Initialize();

        const auto current_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch());
        Settings::values.custom_rtc_differential =
            Settings::values.custom_rtc.value_or(current_time) - current_time;

        // Create a default fs if one doesn't already exist.
        if (virtual_filesystem == nullptr)
            virtual_filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
        if (content_provider == nullptr)
            content_provider = std::make_unique<FileSys::ContentProviderUnion>();

        /// Create default implementations of applets if one is not provided.
        applet_manager.SetDefaultAppletsIfMissing();

        telemetry_session = std::make_unique<Core::TelemetrySession>();
        service_manager = std::make_shared<Service::SM::ServiceManager>();

        Service::Init(service_manager, system, *virtual_filesystem);
        GDBStub::Init();

        renderer = VideoCore::CreateRenderer(emu_window, system);
        if (!renderer->Init()) {
            return ResultStatus::ErrorVideoCore;
        }

        gpu_core = VideoCore::CreateGPU(system);

        is_powered_on = true;

        LOG_DEBUG(Core, "Initialized OK");

        // Reset counters and set time origin to current frame
        GetAndResetPerfStats();
        perf_stats.BeginSystemFrame();

        return ResultStatus::Success;
    }

    ResultStatus Load(System& system, Frontend::EmuWindow& emu_window,
                      const std::string& filepath) {
        app_loader = Loader::GetLoader(GetGameFileFromPath(virtual_filesystem, filepath));
        if (!app_loader) {
            LOG_CRITICAL(Core, "Failed to obtain loader for {}!", filepath);
            return ResultStatus::ErrorGetLoader;
        }

        ResultStatus init_result{Init(system, emu_window)};
        if (init_result != ResultStatus::Success) {
            LOG_CRITICAL(Core, "Failed to initialize system (Error {})!",
                         static_cast<int>(init_result));
            Shutdown();
            return init_result;
        }

        telemetry_session->AddInitialInfo(*app_loader);
        auto main_process = Kernel::Process::Create(system, "main");
        const auto [load_result, load_parameters] = app_loader->Load(*main_process);
        if (load_result != Loader::ResultStatus::Success) {
            LOG_CRITICAL(Core, "Failed to load ROM (Error {})!", static_cast<int>(load_result));
            Shutdown();

            return static_cast<ResultStatus>(static_cast<u32>(ResultStatus::ErrorLoader) +
                                             static_cast<u32>(load_result));
        }
        kernel.MakeCurrentProcess(main_process.get());

        // Main process has been loaded and been made current.
        // Begin GPU and CPU execution.
        gpu_core->Start();
        cpu_core_manager.StartThreads();

        // All threads are started, begin main process execution, now that we're in the clear.
        main_process->Run(load_parameters->main_thread_priority,
                          load_parameters->main_thread_stack_size);

        status = ResultStatus::Success;
        return status;
    }

    void Shutdown() {
        // Log last frame performance stats
        const auto perf_results = GetAndResetPerfStats();
        telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_EmulationSpeed",
                                    perf_results.emulation_speed * 100.0);
        telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_Framerate",
                                    perf_results.game_fps);
        telemetry_session->AddField(Telemetry::FieldType::Performance, "Shutdown_Frametime",
                                    perf_results.frametime * 1000.0);

        is_powered_on = false;

        // Shutdown emulation session
        renderer.reset();
        GDBStub::Shutdown();
        Service::Shutdown();
        service_manager.reset();
        cheat_engine.reset();
        telemetry_session.reset();
        gpu_core.reset();

        // Close all CPU/threading state
        cpu_core_manager.Shutdown();

        // Shutdown kernel and core timing
        kernel.Shutdown();
        core_timing.Shutdown();

        // Close app loader
        app_loader.reset();

        // Clear all applets
        applet_manager.ClearAll();

        LOG_DEBUG(Core, "Shutdown OK");
    }

    Loader::ResultStatus GetGameName(std::string& out) const {
        if (app_loader == nullptr)
            return Loader::ResultStatus::ErrorNotInitialized;
        return app_loader->ReadTitle(out);
    }

    void SetStatus(ResultStatus new_status, const char* details = nullptr) {
        status = new_status;
        if (details) {
            status_details = details;
        }
    }

    PerfStatsResults GetAndResetPerfStats() {
        return perf_stats.GetAndResetStats(core_timing.GetGlobalTimeUs());
    }

    Timing::CoreTiming core_timing;
    Kernel::KernelCore kernel;
    /// RealVfsFilesystem instance
    FileSys::VirtualFilesystem virtual_filesystem;
    /// ContentProviderUnion instance
    std::unique_ptr<FileSys::ContentProviderUnion> content_provider;
    /// AppLoader used to load the current executing application
    std::unique_ptr<Loader::AppLoader> app_loader;
    std::unique_ptr<VideoCore::RendererBase> renderer;
    std::unique_ptr<Tegra::GPU> gpu_core;
    std::shared_ptr<Tegra::DebugContext> debug_context;
    CpuCoreManager cpu_core_manager;
    bool is_powered_on = false;

    std::unique_ptr<FileSys::CheatEngine> cheat_engine;
    std::array<u8, 0x20> build_id{};

    /// Frontend applets
    Service::AM::Applets::AppletManager applet_manager;

    /// Service manager
    std::shared_ptr<Service::SM::ServiceManager> service_manager;

    /// Telemetry session for this emulation session
    std::unique_ptr<Core::TelemetrySession> telemetry_session;

    ResultStatus status = ResultStatus::Success;
    std::string status_details = "";

    Core::PerfStats perf_stats;
    Core::FrameLimiter frame_limiter;
};

System::System() : impl{std::make_unique<Impl>(*this)} {}
System::~System() = default;

Cpu& System::CurrentCpuCore() {
    return impl->CurrentCpuCore();
}

const Cpu& System::CurrentCpuCore() const {
    return impl->CurrentCpuCore();
}

System::ResultStatus System::RunLoop(bool tight_loop) {
    return impl->RunLoop(tight_loop);
}

System::ResultStatus System::SingleStep() {
    return RunLoop(false);
}

void System::InvalidateCpuInstructionCaches() {
    impl->cpu_core_manager.InvalidateAllInstructionCaches();
}

System::ResultStatus System::Load(Frontend::EmuWindow& emu_window, const std::string& filepath) {
    return impl->Load(*this, emu_window, filepath);
}

bool System::IsPoweredOn() const {
    return impl->is_powered_on;
}

void System::PrepareReschedule() {
    CurrentCpuCore().PrepareReschedule();
}

void System::PrepareReschedule(s32 core_index) {
    if (core_index >= 0)
        CpuCore(core_index).PrepareReschedule();
}

PerfStatsResults System::GetAndResetPerfStats() {
    return impl->GetAndResetPerfStats();
}

TelemetrySession& System::TelemetrySession() {
    return *impl->telemetry_session;
}

const TelemetrySession& System::TelemetrySession() const {
    return *impl->telemetry_session;
}

ARM_Interface& System::CurrentArmInterface() {
    return CurrentCpuCore().ArmInterface();
}

const ARM_Interface& System::CurrentArmInterface() const {
    return CurrentCpuCore().ArmInterface();
}

std::size_t System::CurrentCoreIndex() const {
    return CurrentCpuCore().CoreIndex();
}

Kernel::Scheduler& System::CurrentScheduler() {
    return CurrentCpuCore().Scheduler();
}

const Kernel::Scheduler& System::CurrentScheduler() const {
    return CurrentCpuCore().Scheduler();
}

Kernel::Scheduler& System::Scheduler(std::size_t core_index) {
    return CpuCore(core_index).Scheduler();
}

const Kernel::Scheduler& System::Scheduler(std::size_t core_index) const {
    return CpuCore(core_index).Scheduler();
}

/// Gets the global scheduler
Kernel::GlobalScheduler& System::GlobalScheduler() {
    return impl->kernel.GlobalScheduler();
}

/// Gets the global scheduler
const Kernel::GlobalScheduler& System::GlobalScheduler() const {
    return impl->kernel.GlobalScheduler();
}

Kernel::Process* System::CurrentProcess() {
    return impl->kernel.CurrentProcess();
}

const Kernel::Process* System::CurrentProcess() const {
    return impl->kernel.CurrentProcess();
}

ARM_Interface& System::ArmInterface(std::size_t core_index) {
    return CpuCore(core_index).ArmInterface();
}

const ARM_Interface& System::ArmInterface(std::size_t core_index) const {
    return CpuCore(core_index).ArmInterface();
}

Cpu& System::CpuCore(std::size_t core_index) {
    return impl->cpu_core_manager.GetCore(core_index);
}

const Cpu& System::CpuCore(std::size_t core_index) const {
    ASSERT(core_index < NUM_CPU_CORES);
    return impl->cpu_core_manager.GetCore(core_index);
}

ExclusiveMonitor& System::Monitor() {
    return impl->cpu_core_manager.GetExclusiveMonitor();
}

const ExclusiveMonitor& System::Monitor() const {
    return impl->cpu_core_manager.GetExclusiveMonitor();
}

Tegra::GPU& System::GPU() {
    return *impl->gpu_core;
}

const Tegra::GPU& System::GPU() const {
    return *impl->gpu_core;
}

VideoCore::RendererBase& System::Renderer() {
    return *impl->renderer;
}

const VideoCore::RendererBase& System::Renderer() const {
    return *impl->renderer;
}

Kernel::KernelCore& System::Kernel() {
    return impl->kernel;
}

const Kernel::KernelCore& System::Kernel() const {
    return impl->kernel;
}

Timing::CoreTiming& System::CoreTiming() {
    return impl->core_timing;
}

const Timing::CoreTiming& System::CoreTiming() const {
    return impl->core_timing;
}

Core::PerfStats& System::GetPerfStats() {
    return impl->perf_stats;
}

const Core::PerfStats& System::GetPerfStats() const {
    return impl->perf_stats;
}

Core::FrameLimiter& System::FrameLimiter() {
    return impl->frame_limiter;
}

const Core::FrameLimiter& System::FrameLimiter() const {
    return impl->frame_limiter;
}

Loader::ResultStatus System::GetGameName(std::string& out) const {
    return impl->GetGameName(out);
}

void System::SetStatus(ResultStatus new_status, const char* details) {
    impl->SetStatus(new_status, details);
}

const std::string& System::GetStatusDetails() const {
    return impl->status_details;
}

Loader::AppLoader& System::GetAppLoader() const {
    return *impl->app_loader;
}

void System::SetGPUDebugContext(std::shared_ptr<Tegra::DebugContext> context) {
    impl->debug_context = std::move(context);
}

Tegra::DebugContext* System::GetGPUDebugContext() const {
    return impl->debug_context.get();
}

void System::RegisterCheatList(const std::vector<FileSys::CheatList>& list,
                               const std::string& build_id, VAddr code_region_start,
                               VAddr code_region_end) {
    impl->cheat_engine = std::make_unique<FileSys::CheatEngine>(*this, list, build_id,
                                                                code_region_start, code_region_end);
}

void System::SetFilesystem(std::shared_ptr<FileSys::VfsFilesystem> vfs) {
    impl->virtual_filesystem = std::move(vfs);
}

std::shared_ptr<FileSys::VfsFilesystem> System::GetFilesystem() const {
    return impl->virtual_filesystem;
}

void System::SetAppletFrontendSet(Service::AM::Applets::AppletFrontendSet&& set) {
    impl->applet_manager.SetAppletFrontendSet(std::move(set));
}

void System::SetDefaultAppletFrontendSet() {
    impl->applet_manager.SetDefaultAppletFrontendSet();
}

Service::AM::Applets::AppletManager& System::GetAppletManager() {
    return impl->applet_manager;
}

const Service::AM::Applets::AppletManager& System::GetAppletManager() const {
    return impl->applet_manager;
}

void System::SetContentProvider(std::unique_ptr<FileSys::ContentProviderUnion> provider) {
    impl->content_provider = std::move(provider);
}

FileSys::ContentProvider& System::GetContentProvider() {
    return *impl->content_provider;
}

const FileSys::ContentProvider& System::GetContentProvider() const {
    return *impl->content_provider;
}

void System::RegisterContentProvider(FileSys::ContentProviderUnionSlot slot,
                                     FileSys::ContentProvider* provider) {
    impl->content_provider->SetSlot(slot, provider);
}

void System::ClearContentProvider(FileSys::ContentProviderUnionSlot slot) {
    impl->content_provider->ClearSlot(slot);
}

void System::SetCurrentProcessBuildID(std::array<u8, 32> id) {
    impl->build_id = id;
}

const std::array<u8, 32>& System::GetCurrentProcessBuildID() const {
    return impl->build_id;
}

System::ResultStatus System::Init(Frontend::EmuWindow& emu_window) {
    return impl->Init(*this, emu_window);
}

void System::Shutdown() {
    impl->Shutdown();
}

Service::SM::ServiceManager& System::ServiceManager() {
    return *impl->service_manager;
}

const Service::SM::ServiceManager& System::ServiceManager() const {
    return *impl->service_manager;
}

} // namespace Core
