#pragma once
#include <pwd.h>
#include <sys/stat.h>
#include "audio/preferences.h"

/* A separate mode of audiod writes a snapshot without holding up its mixer.
 * The child gets only an explicit stdin pipe, not the parent's PCM or ports. */
#define AUDIOD_PREFERENCE_SAVE_MAGIC 0x53505841u
struct audiod_preference_save {
    uint32_t magic, bytes, master_volume, master_muted;
    char text[SAVANXP_AUDIO_PREFERENCE_TEXT];
};
static int audiod_preference_path(const char* name, unsigned alternative, char* out, size_t capacity) {
    const struct passwd* user = getpwnam("user");
    int count;
    if (!user) return -SAVANXP_ENOENT;
    if (alternative == 0) count = snprintf(out,capacity,"%s/%s/%s",user->pw_dir,SAVANXP_SETTINGS_DIR,name);
    else if (alternative == 1) count = snprintf(out,capacity,"%s/%s/%s.previous",user->pw_dir,SAVANXP_SETTINGS_DIR,name);
    else if (alternative == 2) count = snprintf(out,capacity,"%s/%s",user->pw_dir,name);
    else count = snprintf(out,capacity,"/%s",name);
    return count < 0 || (size_t)count >= capacity ? -SAVANXP_E2BIG : 0;
}
static long audiod_preference_read_file(const char* name, char* text, size_t capacity) {
    unsigned alternative;
    for (alternative = 0; alternative < 4; ++alternative) {
        char path[256]; long fd, got = 0; size_t done = 0;
        int result = audiod_preference_path(name,alternative,path,sizeof(path));
        if (result < 0) return result;
        fd = savanxp_open(path);
        if (fd == -SAVANXP_ENOENT) continue;
        if (fd < 0) return fd;
        while (done < capacity) {
            got = savanxp_read((int)fd,text+done,capacity-done);
            if (got <= 0) break;
            done += (size_t)got;
        }
        (void)savanxp_close((int)fd);
        return got < 0 ? got : (long)done;
    }
    return -SAVANXP_ENOENT;
}
static long audiod_preference_transfer(int fd, void* data, size_t bytes, int writing) {
    size_t done = 0;
    while (done < bytes) {
        long result = writing ? savanxp_write(fd,(const unsigned char*)data+done,bytes-done) :
                                savanxp_read(fd,(unsigned char*)data+done,bytes-done);
        if (result <= 0) return result < 0 ? result : -SAVANXP_EIO;
        done += (size_t)result;
    }
    return 0;
}
static long audiod_preference_write_file(const char* name, const void* data, size_t bytes) {
    const struct passwd* user = getpwnam("user");
    char directory[256], path[256], temporary[288], previous[272];
    struct savanxp_stat old;
    long result, fd;
    int count, moved = 0;
    if (!user) return -SAVANXP_ENOENT;
    result = savanxp_mkdir_mode("/users",0755);
    if (result < 0 && result != -SAVANXP_EEXIST) return result;
    result = savanxp_mkdir_mode(user->pw_dir,0700);
    if (result < 0 && result != -SAVANXP_EEXIST) return result;
    if (!result && (result = savanxp_chown(user->pw_dir,user->pw_uid,user->pw_gid)) < 0) return result;
    count = snprintf(directory,sizeof(directory),"%s/%s",user->pw_dir,SAVANXP_SETTINGS_DIR);
    if (count < 0 || (size_t)count >= sizeof(directory)) return -SAVANXP_E2BIG;
    result = savanxp_mkdir_mode(directory,0700);
    if (result < 0 && result != -SAVANXP_EEXIST) return result;
    if (!result && (result = savanxp_chown(directory,user->pw_uid,user->pw_gid)) < 0) return result;
    result = audiod_preference_path(name,0,path,sizeof(path));
    if (result < 0) return result;
    result = savanxp_getpid();
    if (result <= 0) return -SAVANXP_EIO;
    count = snprintf(temporary,sizeof(temporary),"%s.%u.tmp",path,(uint32_t)result);
    if (count < 0 || (size_t)count >= sizeof(temporary)) return -SAVANXP_E2BIG;
    count = snprintf(previous,sizeof(previous),"%s.previous",path);
    if (count < 0 || (size_t)count >= sizeof(previous)) return -SAVANXP_E2BIG;
    fd = savanxp_open_create(temporary,SAVANXP_OPEN_WRITE|SAVANXP_OPEN_CREATE|SAVANXP_OPEN_TRUNCATE,0600);
    if (fd < 0) return fd;
    result = audiod_preference_transfer((int)fd,(void*)data,bytes,1);
    if (!result) result = savanxp_fchmod((int)fd,0600);
    (void)savanxp_close((int)fd);
    if (!result) result = savanxp_chown(temporary,user->pw_uid,user->pw_gid);
    if (!result) result = savanxp_sync();
    if (!result) {
        result = savanxp_stat(path,&old);
        if (!result) {
            if (!S_ISREG(old.st_mode)) result = -SAVANXP_EINVAL;
            else {
                // SxFS rename cannot replace a destination. Keep a recoverable
                // previous copy before installing the complete new file.
                result = savanxp_unlink(previous);
                if (result == -SAVANXP_ENOENT) result = 0;
                if (!result) { result = savanxp_rename(path,previous); moved = !result; }
            }
        } else if (result == -SAVANXP_ENOENT) result = 0;
    }
    if (!result) result = savanxp_rename(temporary,path);
    if (result < 0) {
        if (moved) (void)savanxp_rename(previous,path);
        (void)savanxp_unlink(temporary);
    }
    return result;
}
static int audiod_preference_save_one(void) {
    static struct audiod_preference_save request;
    static struct savanxp_audio_preferences validated;
    char master[16];
    long got = savanxp_read(0,&request,16);
    long result;
    int bytes;
    if (!got) return -1;
    if (got < 0) return (int)-got;
    result = got == 16 ? 0 : audiod_preference_transfer(0,(unsigned char*)&request+got,16-(size_t)got,0);
    if (result < 0) return (int)-result;
    if (request.magic != AUDIOD_PREFERENCE_SAVE_MAGIC || !request.bytes ||
        request.bytes > sizeof(request.text) || request.master_volume > 100 || request.master_muted > 1)
        return SAVANXP_EINVAL;
    result = audiod_preference_transfer(0,request.text,request.bytes,0);
    if (result < 0) return (int)-result;
    if (!savanxp_audio_preferences_decode(&validated,request.text,request.bytes)) return SAVANXP_EINVAL;
    bytes = snprintf(master,sizeof(master),"%u %u\n",request.master_volume,request.master_muted);
    result = audiod_preference_write_file("audio.cfg",master,(size_t)bytes);
    if (!result) result = audiod_preference_write_file("audio-mixer.cfg",request.text,request.bytes);
    if (!result) result = savanxp_sync();
    if (result < 0) eprintf("audiod: saving sound preferences failed (%s)\n",result_error_string(result));
    return result < 0 ? (int)-result : 0;
}
static int audiod_preference_save_child(void) {
    for (;;) {
        int32_t result = audiod_preference_save_one();
        if (result == -1) return 0;
        if (savanxp_write(1,&result,sizeof(result)) != sizeof(result)) return SAVANXP_EPIPE;
    }
}
