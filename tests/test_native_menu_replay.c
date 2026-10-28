/* Actual sampler/replay with fake devices and monotonic clock. No OS input/game. */
#include <time.h>
#include <stdint.h>
static uint64_t fixture_us=1000000;
static int fixture_clock(clockid_t kind, struct timespec *t) {
    (void)kind;t->tv_sec=(time_t)(fixture_us/1000000);t->tv_nsec=(long)(fixture_us%1000000)*1000;return 0;
}
#define clock_gettime fixture_clock
#define main unused_sampler_contract_main
#include "test_keyboard_sampling_contract.c"
#undef main
#undef clock_gettime
static void new_file(const char *path,const char *text) {
    FILE *f=fopen(path,"wbx");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(!fclose(f));
}
int main(int argc,char **argv) {
    assert(argc==2);char command[2048],recipe[2048],parked[2048];
    assert(snprintf(command,sizeof(command),"%s/menu-command.txt",argv[1])<(int)sizeof(command));
    assert(snprintf(recipe,sizeof(recipe),"%s/menu-down.replay",argv[1])<(int)sizeof(recipe));
    assert(snprintf(parked,sizeof(parked),"%s/consumed.txt",argv[1])<(int)sizeof(parked));
    new_file(recipe,"0 0 128 255 128 128 0 0\n200 0 128 128 128 128 0 0\n");
    _putenv_s("SPARTAN_PAD_FILE",command);_putenv_s("SPARTAN_PAD_REPLAY",recipe);
    clear_fixture();PadData d;sample(&d);assert(d.buttons==0 && d.left_y==128);
    new_file(command,"replay\n");fixture_us+=30000;sample(&d);
    assert(d.buttons==0 && d.left_y==255 && replay_count==2 && injected.buttons==0 && injected.stick[1]==-1);
    puts("PASS existing native replay produces left-stick Down without any keyboard event");
    fixture_us+=199000;sample(&d);assert(d.left_y==255 && d.buttons==0);
    fixture_us+=1000;sample(&d);assert(d.left_y==128 && d.buttons==0);
    puts("PASS explicit neutral sample takes effect at 200 ms on the consumer clock");
    assert(!rename(command,parked));fixture_us+=100000;sample(&d);assert(d.left_y==128 && d.buttons==0);
    puts("PASS supervisor/control-file disappearance after acceptance cannot extend this replay pulse");
    fixture_us+=1000000;sample(&d);assert(replay_armed==2 && d.left_y==128 && d.buttons==0);
    for(int i=0;i<10;++i){fixture_us+=1000000;sample(&d);assert(replay_armed==2 && d.left_y==128 && d.buttons==0);}
    puts("PASS absent trigger does not rearm; subsequent neutral host samples stay neutral");
    replay_armed=1;replay_start=fixture_us-1000000;replay_next=0;sample(&d);
    assert(replay_armed==2 && d.left_y==128 && d.buttons==0);
    puts("PASS a late resumed sample does not replay the expired Down state");
    gamepad=(SDL_Gamepad *)&fixture_pad_token;fixture_pad_axes[SDL_GAMEPAD_AXIS_LEFTY]=12000;sample(&d);
    assert(d.left_y==axis(12000));
    puts("PASS documented limit: completed replay restores physical host input, not an exclusive neutral controller");
    puts("6 native sampler/replay scenarios passed; no OS input, game or saves");
    return 0;
}
