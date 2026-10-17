// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_notice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>

#include "diagnostics.h"
#include "prosperoeden/version.h"
#include "storage_paths.h"
#include "update_check/self_update.h"
#include "update_check/update_check.h"
#ifdef EDEN_DEV_PROFILE
#include <fstream>
#endif

// The kit's paths, the app's own (update_check/eden_paths.h): after elevation /app0 and /download0
// are gone, so they follow storage_paths.h.
extern "C" const char* eden_self_update_path(int which) {
    static const std::string helper = Eden::AppFile("self-updater.elf");
    static const std::string param = Eden::AppFile("sce_sys/param.json");
    static const std::string sequence = Eden::ConfigFile("self-update-sequence");
    return which == 0 ? helper.c_str() : which == 1 ? param.c_str() : sequence.c_str();
}

namespace Eden::UpdateNotice {
namespace {
// libcurl and OpenSSL want more stack than a default thread of the console has.
constexpr std::size_t kStackSize = 1024 * 1024;

std::mutex lock;
bool started = false;
bool found = false;  // an offer nobody has been told about yet
self_update_check_result answer = SELF_UPDATE_UNKNOWN;
self_update_offer offer{};
self_update_job job{};  // zero until the first Begin, as the kit asks
bool begun = false;

const char* ResultName(self_update_check_result result) {
    static const char* const names[] = {"available", "up-to-date", "unknown", "untrusted", "not-installable"};
    return static_cast<unsigned>(result) < 5 ? names[result] : "?";
}

#ifdef EDEN_DEV_PROFILE
// Development: update-offer.txt in the app folder replaces the catalog's answer (it skips the
// catalog's signature), so the update can be tried before the catalog lists a newer release. Five
// lines, as the boilerplate's example takes them: the new content version, the release's name,
// its ZIP on GitHub, its SHA-256, its size in bytes; any further lines are the release notes.
bool DevelopmentOffer(self_update_offer& out) {
    std::ifstream file(AppFile("update-offer.txt"));
    std::string available, version, artifact, sha256, size;
    if (!file || !std::getline(file, available) || !std::getline(file, version) || !std::getline(file, artifact) ||
        !std::getline(file, sha256) || !std::getline(file, size))
        return false;
    self_update_offer filled{};
    if (!update_check_read_param(AppFile("sce_sys/param.json").c_str(), filled.title, filled.installed)) return false;
    std::snprintf(filled.name, sizeof(filled.name), "ProsperoEden");
    std::snprintf(filled.available, sizeof(filled.available), "%s", available.c_str());
    std::snprintf(filled.version, sizeof(filled.version), "%s", version.c_str());
    std::snprintf(filled.artifact, sizeof(filled.artifact), "%s", artifact.c_str());
    std::snprintf(filled.sha256, sizeof(filled.sha256), "%s", sha256.c_str());
    filled.size = std::strtoull(size.c_str(), nullptr, 10);
    std::string notes, line;
    while (std::getline(file, line)) notes += (notes.empty() ? "" : "\n") + line;
    std::snprintf(filled.notes, sizeof(filled.notes), "%s", notes.c_str());
    // Like the catalog, only a newer version is offered (content versions compare as text).
    if (std::strcmp(filled.available, filled.installed) <= 0) return false;
    out = filled;
    return true;
}
#endif

void* Check(void*) {
    self_update_offer result{};
    self_update_check_result state = self_update_check_self(&result);
#ifdef EDEN_DEV_PROFILE
    if (DevelopmentOffer(result)) {
        Report("update check", "Development: update-offer.txt replaces the catalog's answer");
        state = SELF_UPDATE_AVAILABLE;
    }
#endif
    // The program's own version counts too (issue 230): an install that replaced the program but
    // kept an older sce_sys/param.json read as that older version, and the version the console
    // already ran was offered at every start.
    int comparable = 0;
    if ((state == SELF_UPDATE_AVAILABLE || state == SELF_UPDATE_NOT_INSTALLABLE) &&
        update_check_version_compare(result.available, kAppVersion, &comparable) <= 0 && comparable) {
        Report("update check", (std::string{"sce_sys/param.json names "} + (result.installed[0] ? result.installed : "-") +
                                ", the program is " + kAppVersion + ": nothing newer to offer")
                                   .c_str());
        state = SELF_UPDATE_UP_TO_DATE;
    }
    char line[300];
    std::snprintf(line, sizeof(line), "result=%s installed=%s available=%s version=%s size=%llu",
                  ResultName(state), result.installed[0] ? result.installed : "-",
                  result.available[0] ? result.available : "-", result.version[0] ? result.version : "-",
                  static_cast<unsigned long long>(result.size));
    Report("update check", line);
    if (state == SELF_UPDATE_AVAILABLE || state == SELF_UPDATE_NOT_INSTALLABLE) {
        const std::lock_guard guard(lock);
        answer = state;
        offer = result;
        found = true;
    }
    return nullptr;
}

Phase FromKit(self_update_phase phase) {
    switch (phase) {
    case SELF_UPDATE_STARTING: return Phase::starting;
    case SELF_UPDATE_DOWNLOADING: return Phase::downloading;
    case SELF_UPDATE_UNPACKING: return Phase::unpacking;
    case SELF_UPDATE_READY: return Phase::ready;
    case SELF_UPDATE_APPLYING: return Phase::applying;
    case SELF_UPDATE_CANCELLED: return Phase::cancelled;
    case SELF_UPDATE_FAILED: return Phase::failed;
    default: return Phase::idle;
    }
}
} // namespace

void Start() {
    {
        const std::lock_guard guard(lock);
        if (started) return;
        started = true;
    }
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return;
    pthread_attr_setstacksize(&attributes, kStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, Check, nullptr) != 0)
        Report("update check", "The thread could not start");
    pthread_attr_destroy(&attributes);
}

bool Take(Offer* out) {
    const std::lock_guard guard(lock);
    if (!found) return false;
    found = false;
    if (out) {
        out->installable = answer == SELF_UPDATE_AVAILABLE;
        out->version = offer.version[0] != '\0' ? offer.version : offer.available;
        out->available = offer.available;
        out->size = offer.size;
        out->notes = offer.notes;
        out->notes_truncated = offer.notes_truncated != 0;
    }
    return true;
}

bool Begin() {
    const std::lock_guard guard(lock);
    if (answer != SELF_UPDATE_AVAILABLE) return false;
    if (begun) self_update_finish(&job);
    begun = self_update_start(&job, self_update_console(), &offer) == 1;
    Report("update", begun ? ("Updating to " + std::string(offer.available)).c_str() : "The update could not begin");
    return begun;
}

Progress Poll() {
    Progress progress;
    const std::lock_guard guard(lock);
    if (!begun) return progress;
    self_update_status status{};
    self_update_poll(&job, &status);
    progress.phase = FromKit(status.phase);
    progress.done = status.done;
    progress.total = status.total;
    progress.time_left = status.time_left;
    progress.error = status.error;
    return progress;
}

void Cancel() {
    const std::lock_guard guard(lock);
    if (begun) self_update_cancel(&job);
}

bool Apply() {
    const std::lock_guard guard(lock);
    if (!begun) return false;
    const bool going = self_update_apply(&job) == 1;
    Report("update", going ? "Staged: the helper replaces the files once ProsperoEden has closed"
                           : "The helper did not take the go-ahead");
    return going;
}

void Finish() {
    const std::lock_guard guard(lock);
    if (!begun) return;
    self_update_finish(&job);
    begun = false;
}
} // namespace Eden::UpdateNotice
