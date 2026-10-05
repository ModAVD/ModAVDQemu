// Copyright 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <map>     // for map
#include <memory>  // for unique_ptr
#include <string>  // for basic_string, operator<
#include <string_view>
#include <thread>
#include <vector>  // for vector<>::iterator

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "aemu/base/Compiler.h"  // for DISALLOW_COPY_AND_ASSIGN
#include "aemu/base/async/RecurrentTask.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/files/PathUtils.h"      // for pj, PathUtils
#include "aemu/base/logging/Log.h"          // for dinfo, dwarning
#include "aemu/base/memory/LazyInstance.h"  // for LazyInstance, LAZY_IN...
#include "aemu/base/system/Win32UnicodeString.h"
#include "android/base/system/System.h"         // for System
#include "android/crashreport/CrashConsent.h"   // for CrashReportDatabase
#include "android/crashreport/CrashReporter.h"  // for CrashReporter
#include "android/crashreport/CrashpadLogSink.h"
#include "android/crashreport/HangDetector.h"  // for HangDetector
#include "android/crashreport/Uploader.h"
#include "android/version.h"                     // for EMULATOR_FULL_VERSION...
#include "client/crash_report_database.h"        // for CrashReportDatabase::...
#include "client/crashpad_client.h"              // for CrashpadClient
#include "client/settings.h"                     // for Settings
#include "mini_chromium/base/files/file_path.h"  // for FilePath
#include "util/misc/uuid.h"                      // for UUID

#ifdef _WIN32
#include <io.h>
#endif

using android::base::LazyInstance;
using android::base::PathUtils;
using android::base::pj;
using android::base::RecurrentTask;
using android::base::System;
using crashpad::CrashReportDatabase;

namespace android {
namespace crashreport {

#ifdef NDEBUG
constexpr char CrashURL[] = "https://clients2.google.com/cr/report";
#else
constexpr char CrashURL[] = "https://clients2.google.com/cr/staging_report";
#endif
// The crashpad handler binary, as shipped with the emulator.
const constexpr char kCrashpadHandler[] = "crashpad_handler";

class CrashSystem {
public:
    CrashSystem()
        : mClient(new crashpad::CrashpadClient()),
          mConsentProvider(consentProvider()) {}

    // Gets a handle to single instance of crash reporter
    static CrashSystem* get();

    bool active() const { return mInitialized; }

    bool initialize() {
        if (mInitialized) {
            return false;
        }
        auto handler_path = ::base::FilePath(
                PathUtils::asUnicodePath(
                        System::get()
                                ->findBundledExecutable(kCrashpadHandler)
                                .data())
                        .c_str());
        if (handler_path.empty()) {
            dwarning("Crash handler not found, crash reporting disabled.");
            return false;
        }
        auto database_directory =
                System::get()->envGet("ANDROID_EMU_CRASH_REPORTING_DATABASE");
        mDatabasePath =
                database_directory.empty()
                        ? CrashReporter::databaseDirectory()
                        : ::base::FilePath(PathUtils::asUnicodePath(
                                                   database_directory.c_str())
                                                   .c_str());

        auto metrics_path = ::base::FilePath();
        auto annotations = std::map<std::string, std::string>{
                {"prod", "AndroidEmulator"},
                {"ver", EMULATOR_FULL_VERSION_STRING}};

        std::vector<std::string> args = {"--no-rate-limit"};
#ifdef __APPLE__
        args.emplace_back(
                absl::StrCat("--monitor-pid=", System::get()->getCurrentProcessId()));
#endif
        bool active = mClient->StartHandler(
                handler_path, mDatabasePath, metrics_path, CrashURL, annotations,
               std::move(args) , true, false);

        mDatabase = CrashReportDatabase::Initialize(mDatabasePath);
        mInitialized = active && mDatabase;

        if (mDatabase && mDatabase->GetSettings()) {
            mDatabase->GetSettings()->SetUploadsEnabled(false);
        }
        if (mInitialized) {
            CrashReporter::get()->initialize();
        }

        return mInitialized;
    }

    void uploadEntries() {
        if (!mDatabase || !mInitialized) {
            dinfo("Crash database unavailable, or not initialized, we will not "
                  "report any crashes.");
            return;
        }

#ifdef _WIN32
        android::base::Win32UnicodeString wstr(mDatabasePath.value().c_str());
        std::string message = wstr.toString();
#else
        std::string message = mDatabasePath.value();
#endif
        dinfo("Storing crashdata in: %s, detection is %s for process: %d",
              message, mInitialized ? "enabled" : "disabled",
              System::get()->getCurrentProcessId());

        auto collect = mConsentProvider->consentRequired();
        bool areUploadsEnabled = (collect == CrashConsent::Consent::ALWAYS);
        if (mDatabase && areUploadsEnabled) {
            dinfo("Crash reports will be automatically uploaded to: %s",
                  CrashURL);
            mDatabase->GetSettings()->SetUploadsEnabled(areUploadsEnabled);
        }

        // Get consent for any report that was not yet uploaded.
        std::vector<crashpad::UUID> toRemove;
        std::vector<CrashReportDatabase::Report> reports;
        std::vector<CrashReportDatabase::Report> pendingReports;
        mDatabase->GetCompletedReports(&reports);
        mDatabase->GetPendingReports(&pendingReports);
        reports.insert(reports.end(), pendingReports.begin(),
                       pendingReports.end());

        // MAS: without explicit upload consent, never ask (no dialog), never
        // upload and never delete a report just for lack of consent. Keep the
        // newest kLocalReportsToKeep minidumps on disk for the user to collect
        // and drop only older ones.
        if (!areUploadsEnabled) {
            keepReportsLocally(reports);
            return;
        }

        for (const auto& report : reports) {
            if (!report.uploaded) {
                auto status = mConsentProvider->requestConsent(report);
                switch (status) {
                    case CrashConsent::ReportAction::UPLOAD_REMOVE: {
                        std::thread upload(
                                [this, report]() { processReport(report); });
                        upload.detach();
                        break;
                    }
                    case CrashConsent::ReportAction::REMOVE:
                        dinfo("No consent for crashreport %s, deleting.",
                              report.uuid.ToString().c_str());
                        toRemove.push_back(report.uuid);
                        break;
                    case CrashConsent::ReportAction::UNDECIDED_KEEP:
                        dinfo("Failed to get consent, keeping %s for now.",
                              report.id.c_str());
                        break;
                }
            } else {
                toRemove.push_back(report.uuid);
            }
        }

        for (const auto& report : toRemove) {
            mDatabase->DeleteReport(report);
        }
    }

    void setConsentProvider(CrashConsent* replacement) {
        mConsentProvider.reset(replacement);
    }

private:
    // Number of crash reports kept on disk when uploads are not enabled.
    // ANDROID_EMU_CRASH_REPORTS_TO_KEEP overrides it (1..100).
    static constexpr size_t kLocalReportsToKeep = 5;

    void keepReportsLocally(std::vector<CrashReportDatabase::Report> reports) {
        size_t keep = kLocalReportsToKeep;
        auto env = System::get()->envGet("ANDROID_EMU_CRASH_REPORTS_TO_KEEP");
        if (!env.empty()) {
            int n = atoi(env.c_str());
            if (n >= 1 && n <= 100) {
                keep = n;
            }
        }
        // Uploaded reports are not ours to keep; upstream removes them too.
        std::vector<crashpad::UUID> toRemove;
        std::vector<CrashReportDatabase::Report> local;
        for (const auto& report : reports) {
            if (report.uploaded) {
                toRemove.push_back(report.uuid);
            } else {
                local.push_back(report);
            }
        }
        std::sort(local.begin(), local.end(),
                  [](const CrashReportDatabase::Report& a,
                     const CrashReportDatabase::Report& b) {
                      return a.creation_time > b.creation_time;
                  });
        for (size_t i = 0; i < local.size(); i++) {
#ifdef _WIN32
            std::string path = android::base::Win32UnicodeString(
                                       local[i].file_path.value().c_str())
                                       .toString();
#else
            std::string path = local[i].file_path.value();
#endif
            if (i < keep) {
                dinfo("No upload consent, keeping crash report %s locally "
                      "(not uploaded): %s",
                      local[i].uuid.ToString().c_str(), path.c_str());
            } else {
                dinfo("Keeping only the newest %d crash reports, removing "
                      "older report %s: %s",
                      (int)keep, local[i].uuid.ToString().c_str(),
                      path.c_str());
                toRemove.push_back(local[i].uuid);
            }
        }
        for (const auto& uuid : toRemove) {
            mDatabase->DeleteReport(uuid);
        }
    }

    void processReport(const CrashReportDatabase::Report& report) {
        using namespace std::chrono_literals;
        std::vector<std::chrono::seconds> backoff{0s,  2s,  4s,   8s,  16s,
                                                  32s, 64s, 128s, 256s};
        auto status = UploadResult::kSuccess;
        auto wait = backoff.begin();

        mDatabase->GetSettings()->SetUploadsEnabled(true);
        mDatabase->RequestUpload(report.uuid);
        CrashReportDatabase::Report updatedReport;

        do {
            std::this_thread::sleep_for(*wait);
            mDatabase->LookUpCrashReport(report.uuid, &updatedReport);
            dinfo("Attempting to send crashreport %s to "
                  "%s",
                  updatedReport.uuid.ToString().c_str(), CrashURL);
            if (!updatedReport.uploaded) {
                status = ProcessPendingReport(mDatabase.get(), report);
            } else {
                status = UploadResult::kSuccess;
            }
            ++wait;
        } while (status == UploadResult::kRetry && wait != backoff.end());

        if (status == UploadResult::kSuccess) {
            // We should have a report id, fetch it!

            if (mDatabase->LookUpCrashReport(report.uuid, &updatedReport) ==
                CrashReportDatabase::OperationStatus::kNoError) {
                mConsentProvider->reportCompleted(updatedReport);
            }
        } else {
            dwarning("Failed to send report..");
        }
    }

    DISALLOW_COPY_AND_ASSIGN(CrashSystem);

    std::unique_ptr<crashpad::CrashpadClient> mClient;
    std::unique_ptr<CrashReportDatabase> mDatabase;
    std::unique_ptr<CrashConsent> mConsentProvider;
    ::base::FilePath mDatabasePath;
    bool mInitialized{false};
};

LazyInstance<CrashSystem> sCrashSystem = LAZY_INSTANCE_INIT;

CrashSystem* CrashSystem::get() {
    return sCrashSystem.ptr();
}

bool inject_consent_provider(CrashConsent* myProvider) {
    auto crashSystem = CrashSystem::get();
    crashSystem->setConsentProvider(myProvider);
    return crashSystem->initialize();
}

}  // namespace crashreport
}  // namespace android

extern "C" {
using android::crashreport::CrashSystem;

bool crashhandler_init(int argc, char** argv) {
    bool enableCrashReporting = System::get()->getEnableCrashReporting();

    // Parse command line manually as this can be called early in the process.
    static constexpr std::string_view crashReportModeArg = "-crash-report-mode";
    for (int i = 1; i < argc-1; ++i) {
        if (crashReportModeArg == argv[i]) {
            // '-crash-report-mode disabled' works similar to
            // ANDROID_EMU_ENABLE_CRASH_REPORTING=NO
            const std::string crashReportMode = argv[i+1];
            if (crashReportMode == "disabled") {
                enableCrashReporting = false;
            }
            break;
        }
    }

    if (!enableCrashReporting) {
        dinfo("Crashreporting disabled, not reporting crashes.");
        return false;
    }
    android::crashreport::CrashpadLogSink::registerSink();

    // Catch crashes in everything.
    // This promises to not launch any threads...
    if (!CrashSystem::get()->initialize()) {
        return false;
    }

    const auto reporter = android::crashreport::CrashReporter::get();

    // Make sure we don't report any hangs until all related loopers
    // actually get started.
    reporter->hangDetector().pause(true);

    std::string arguments;
    arguments.reserve(1024);

    // Add command line arguments to the crash reports. Skip the first
    // one to not add user path to the qemu binary and improve readability
    for (int i = 1; i < argc; i++) {
        absl::StrAppend(&arguments, argv[i]);
        absl::StrAppend(&arguments, " ");
    }
    reporter->attachData("command_line", arguments);

    // Add important environment variables to the crash reports
    // Do not add all of them to reduce possible PII exposure
    const std::vector<std::string_view> envPrefixes = {
        "ANDROID_EMU", // covers both ANDROID_EMU_ and ANDROID_EMULATOR_
        "GFXSTREAM_",
    };
    const std::vector<std::string_view> envExcludes = {
        "ANDROID_EMULATOR_WRAPPER_PID",
        "ANDROID_EMULATOR_LAUNCHER_DIR",
        "ANDROID_EMULATOR_PREBUILTS_DIR",
        "ANDROID_EMULATOR_DISCOVERY_DIR",
    };
    const auto allEnv = System::get()->envGetAll();
    arguments.clear();
    for (const std::string& env : allEnv) {
        const bool shouldReport =
                std::any_of(envPrefixes.begin(), envPrefixes.end(),
                            [&](std::string_view p) {
                                return absl::StartsWith(env, p);
                            }) &&
                std::none_of(envExcludes.begin(), envExcludes.end(),
                             [&](std::string_view p) {
                                 return absl::StartsWith(env, p);
                             });

        if (shouldReport) {
            absl::StrAppend(&arguments, env);
            absl::StrAppend(&arguments, "\n");
        }
    }

    reporter->attachData("environment_vars", arguments);

    return true;
}

void upload_crashes(void) {
    const auto reporter = CrashSystem::get();
    if (reporter && reporter->active()) {
        reporter->uploadEntries();
    }
}
}
