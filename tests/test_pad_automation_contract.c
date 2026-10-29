/* Characterize existing scripted input without a window, controller or game.
 * This does not enable automation and does not change runtime behavior. */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif
#include <utime.h>

int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    (void)address; (void)size; assert(!"switch attacks must remain disabled"); return 0;
}

int spartanportgpu_overlay_captures_input(void) { return 0; }
int spartanportgpu_vibration_percent(void) { return 0; }
void spartanportgpu_engine_fps_cycle(void) {}
int spartanportgpu_mouse_prompt_distance(void) { return 48; }
void spartanportgpu_mouse_pause_key(void) {}
int spartanportgpu_mouse_aim_style(void) { return 0; }
int spartanportgpu_mouse_aim_sensitivity(void) { return 100; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name; return 0;
}

static void write_at(const char *path, const char *text, time_t stamp) {
    FILE *f=fopen(path,"wb");
    assert(f && fwrite(text,1,strlen(text),f)==strlen(text));
    assert(fclose(f)==0);
    struct utimbuf times={stamp,stamp};
    assert(utime(path,&times)==0);
    usleep(25000);
}

int main(int argc, char **argv) {
    assert(argc==2);
    char path[2048], parked[2048], recipe[2048];
    assert(snprintf(path,sizeof(path),"%s/pad.txt",argv[1])<(int)sizeof(path));
    assert(snprintf(parked,sizeof(parked),"%s/parked-pad.txt",argv[1])<(int)sizeof(parked));
    assert(snprintf(recipe,sizeof(recipe),"%s/replay.txt",argv[1])<(int)sizeof(recipe));
    struct stat st;
    assert(stat(path,&st)!=0 && stat(parked,&st)!=0 && stat(recipe,&st)!=0);
    setenv("SPARTAN_PAD_FILE",path,1);
    write_at(path,"lx=0",1700000000);
    read_inject();
    assert(injected.stick[0]==0);
    write_at(path,"lx=9",1700000000);
    read_inject();
#ifdef _WIN32
    assert(injected.stick[0]==0);
    puts("PASS observed limitation: equal-size edits within one Windows mtime second are not observed");
#endif
    write_at(path,"lx=9 ",1700000000);
    read_inject();
    assert(injected.stick[0]==9);
    puts("PASS changed file size makes the command observable");
    write_at(path,"cross lx=9",1700000000);
    read_inject();
    assert(injected.buttons==BTN_CROSS && injected.stick[0]==9);
    assert(rename(path,parked)==0);
    usleep(25000);
    read_inject();
    assert(injected.buttons==BTN_CROSS && injected.stick[0]==9);
    puts("PASS observed limitation: a missing command file preserves held input; no lease or expiry");
    write_at(path,"",1700000001);
    read_inject();
    assert(injected.buttons==0 && injected.stick[0]==-1);
    puts("PASS an observed empty file releases injected buttons and axes");
    write_at(recipe,"0 0 128 80 128 128 0 0\n250 0 128 128 128 128 0 0\n",1700000000);
    setenv("SPARTAN_PAD_REPLAY",recipe,1);
    PadData data={0};
    replay_armed=1;
    replay_start=0;
    replay_sample(&data);
    assert(data.left_y==80 && replay_count==2);
    replay_start=now_us()-300000;
    replay_sample(&data);
    assert(data.buttons==0 && data.left_y==128);
    puts("PASS bounded replay reaches its explicit neutral final sample");
    replay_start=now_us()-800000;
    data.buttons=BTN_SQUARE;
    data.left_y=140;
    replay_sample(&data);
    assert(replay_armed==2 && data.buttons==BTN_SQUARE && data.left_y==140);
    puts("PASS observed limitation: replay completion returns to host input, not an exclusive neutral controller");
    write_at(recipe,"0 0 128 200 128 128 0 0\n250 0 128 128 128 128 0 0\n",1700000002);
    replay_armed=1;
    replay_start=0;
    replay_sample(&data);
    assert(data.left_y==80);
    puts("PASS observed limitation: replay recipe is loaded once per process and cannot be changed per phase");
    return 0;
}
