#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <pwd.h>
#include "abi/savanxp_abi.h"
#include <sys/stat.h>

// Exercise the actual writer against a filesystem with SxFS's non-replacing
// rename semantics. Failures are injected at the durability/installation edges.
namespace {
struct Node { std::string data; unsigned mode = S_IFREG|0600, uid = 0, gid = 0; };
struct Handle { std::string path; size_t offset = 0; };
std::map<std::string,Node> files;
std::map<int,Handle> handles;
std::string input, output;
size_t input_offset;
int next_fd = 10, writes_before_failure = -1, syncs_before_failure = -1;
bool fail_install, fail_rotate;
unsigned sync_count;
constexpr const char* canonical = "/users/user/Settings/audio.cfg";
constexpr const char* previous = "/users/user/Settings/audio.cfg.previous";
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
passwd* test_user(const char*) {
    static char name[] = "user", home[] = "/users/user";
    static passwd user = {};
    user.pw_name = name; user.pw_dir = home; user.pw_uid = user.pw_gid = 1000;
    return &user;
}
long savanxp_open(const char* path) {
    if (!files.count(path)) return -SAVANXP_ENOENT;
    handles[next_fd] = {path,0}; return next_fd++;
}
long savanxp_open_create(const char* path, unsigned long, unsigned long mode) {
    files[path] = {"",unsigned(S_IFREG|mode),0,0}; return savanxp_open(path);
}
long savanxp_close(int fd) { handles.erase(fd); return 0; }
long savanxp_read(int fd, void* data, size_t count) {
    auto& text = fd == 0 ? input : files.at(handles.at(fd).path).data;
    auto& offset = fd == 0 ? input_offset : handles.at(fd).offset;
    // Deliberately fragment reads to test request/file assembly.
    count = std::min({count,text.size()-offset,size_t(7)});
    std::memcpy(data,text.data()+offset,count); offset += count; return long(count);
}
long savanxp_write(int fd, const void* data, size_t count) {
    if (!writes_before_failure--) return -SAVANXP_ENOSPC;
    auto& text = fd == 1 ? output : files.at(handles.at(fd).path).data;
    text.append(static_cast<const char*>(data),count); return long(count);
}
long savanxp_sync() {
    ++sync_count;
    return !syncs_before_failure-- ? -SAVANXP_EIO : 0;
}
long savanxp_getpid() { return 42; }
long test_stat(const char* path, struct savanxp_stat* info) {
    if (!files.count(path)) return -SAVANXP_ENOENT;
    info->st_mode = files.at(path).mode; return 0;
}
long savanxp_mkdir_mode(const char* path, unsigned long mode) {
    if (files.count(path)) return -SAVANXP_EEXIST;
    files[path] = {"",unsigned(S_IFDIR|mode),0,0}; return 0;
}
long savanxp_chown(const char* path, unsigned long uid, unsigned long gid) {
    files.at(path).uid = unsigned(uid); files.at(path).gid = unsigned(gid); return 0;
}
long savanxp_fchmod(int fd, unsigned long mode) {
    files.at(handles.at(fd).path).mode = S_IFREG|unsigned(mode); return 0;
}
long savanxp_unlink(const char* path) { return files.erase(path) ? 0 : -SAVANXP_ENOENT; }
long savanxp_rename(const char* old_path, const char* new_path) {
    if (!files.count(old_path)) return -SAVANXP_ENOENT;
    if (files.count(new_path)) return -SAVANXP_EEXIST;
    if ((fail_install && std::strstr(old_path,".tmp")) || (fail_rotate && !std::strcmp(old_path,canonical)))
        return -SAVANXP_EIO;
    files[new_path] = files.at(old_path); files.erase(old_path); return 0;
}
void eprintf(const char*, ...) {}
const char* result_error_string(long) { return "injected error"; }
constexpr const char* SAVANXP_SETTINGS_DIR = "Settings";
}
#define getpwnam test_user
#define savanxp_stat(...) test_stat(__VA_ARGS__)
#include "../../subsystems/posix/userland/audiod_preferences_io.h"
#undef getpwnam
#undef savanxp_stat

static void reset() {
    files.clear(); handles.clear(); input.clear(); output.clear(); input_offset = 0;
    writes_before_failure = syncs_before_failure = -1; fail_install = fail_rotate = false; sync_count = 0;
    files[canonical] = {"old\n",S_IFREG|0600,1000,1000};
    files[previous] = {"older\n",S_IFREG|0600,1000,1000};
}
static std::string request() {
    savanxp_audio_preferences preferences;
    savanxp_audio_preferences_init(&preferences);
    audiod_preference_save save = {};
    save.magic = AUDIOD_PREFERENCE_SAVE_MAGIC; save.master_volume = 62; save.master_muted = 1;
    save.bytes = unsigned(savanxp_audio_preferences_encode(&preferences,save.text,sizeof(save.text)));
    return {reinterpret_cast<const char*>(&save),16+save.bytes};
}
int main() {
    reset();
    check(!audiod_preference_write_file("audio.cfg","new\n",4),"replace a complete existing file");
    check(files.at(canonical).data == "new\n" && files.at(previous).data == "old\n","keep last complete canonical as previous");
    check(files.at(canonical).mode == (S_IFREG|0600) && files.at(canonical).uid == 1000 &&
          files.at(canonical).gid == 1000,"private file belongs to the profile user");
    for (unsigned failure = 0; failure < 4; ++failure) {
        reset();
        if (failure == 0) writes_before_failure = 0;
        if (failure == 1) syncs_before_failure = 0;
        if (failure == 2) fail_rotate = true;
        if (failure == 3) fail_install = true;
        check(audiod_preference_write_file("audio.cfg","new\n",4) < 0,"report failed write/sync/rotation/install");
        check(files.at(canonical).data == "old\n","failure preserves/restores old canonical");
        check(!files.count(std::string(canonical)+".42.tmp"),"failed snapshot leaves no temporary file");
    }
    reset(); files.erase(canonical);
    char text[64] = {};
    check(audiod_preference_read_file("audio.cfg",text,sizeof(text)) == 6 && !std::memcmp(text,"older\n",6),
          "missing canonical recovers previous before legacy locations");
    reset(); files[canonical].data = "broken";
    check(audiod_preference_read_file("audio.cfg",text,sizeof(text)) == 6 && !std::memcmp(text,"broken",6),
          "malformed canonical is surfaced to strict parser, never silently replaced by older state");
    reset(); input = request();
    check(!audiod_preference_save_child() && output.size() == sizeof(int32_t),"worker returns one explicit result and exits at EOF");
    int32_t status = -1; std::memcpy(&status,output.data(),sizeof(status));
    check(status == 0 && sync_count == 3 && files.at(canonical).data == "62 1\n", "successful worker acknowledges after all three durability points");
    reset(); input = request(); syncs_before_failure = 2;
    check(!audiod_preference_save_child(),"worker survives failed final sync until parent closes pipe");
    std::memcpy(&status,output.data(),sizeof(status));
    check(status == SAVANXP_EIO,"failed final sync never acknowledges durability");
    reset(); input = request(); input.back() = '?';
    check(audiod_preference_save_one() == SAVANXP_EINVAL && files.at(canonical).data == "old\n", "malformed request cannot write preferences");
    reset(); input = request().substr(0,20);
    check(audiod_preference_save_one() == SAVANXP_EIO && files.at(canonical).data == "old\n", "truncated request cannot write preferences");
    reset(); input = request()+request();
    check(!audiod_preference_save_child() && output.size() == 2*sizeof(int32_t),"persistent worker accepts consecutive complete snapshots");
    std::puts("audio-preference-io-test: PASS (replacement, ownership, rollback, durability, malformed requests and persistent worker)");
}
