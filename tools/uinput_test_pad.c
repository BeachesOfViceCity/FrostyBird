/* Validation fixture only. A Linux hid-playstation-layout gamepad; all
 * input and force feedback belong to this new uinput fd, not to a real pad. */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fail(const char *s){perror(s);exit(1);}
static void emit(int fd,unsigned type,unsigned code,int value){
    struct input_event e={.type=type,.code=code,.value=value};
    if(write(fd,&e,sizeof(e))!=(ssize_t)sizeof(e))fail("write uinput");
}
static void feedback(int fd,struct input_event e){
    if(e.type==EV_UINPUT && e.code==UI_FF_UPLOAD){
        struct uinput_ff_upload u={.request_id=e.value};
        if(ioctl(fd,UI_BEGIN_FF_UPLOAD,&u)<0)fail("begin upload");
        if(u.effect.type==FF_RUMBLE)printf("RUMBLE %u %u\n",u.effect.u.rumble.strong_magnitude,u.effect.u.rumble.weak_magnitude);
        else u.retval=-EINVAL;
        if(ioctl(fd,UI_END_FF_UPLOAD,&u)<0)fail("end upload");
    }else if(e.type==EV_UINPUT && e.code==UI_FF_ERASE){
        struct uinput_ff_erase u={.request_id=e.value};
        if(ioctl(fd,UI_BEGIN_FF_ERASE,&u)<0 || ioctl(fd,UI_END_FF_ERASE,&u)<0)fail("erase");
    }else if(e.type==EV_FF)printf("PLAY %u %d\n",e.code,e.value);
}
int main(int argc,char **argv){
    if(argc!=2 || (strcmp(argv[1],"usb") && strcmp(argv[1],"bluetooth")))return 2;
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stdin,NULL,_IONBF,0);
    int fd=open("/dev/uinput",O_RDWR|O_NONBLOCK);if(fd<0)fail("open /dev/uinput");
    int usb=!strcmp(argv[1],"usb");
    struct uinput_setup u={.id={.bustype=usb?BUS_USB:BUS_BLUETOOTH,.vendor=0x054c,.product=0x0df2,.version=usb?0x8111:0x8100},
                           .name="XS DualSense layout fixture",.ff_effects_max=16};
    const int keys[]={BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,BTN_TL,BTN_TR,BTN_TL2,BTN_TR2,
                      BTN_SELECT,BTN_START,BTN_MODE,BTN_THUMBL,BTN_THUMBR};
    if(ioctl(fd,UI_SET_EVBIT,EV_KEY)<0 || ioctl(fd,UI_SET_EVBIT,EV_ABS)<0 ||
       ioctl(fd,UI_SET_EVBIT,EV_FF)<0 || ioctl(fd,UI_SET_FFBIT,FF_RUMBLE)<0)fail("capabilities");
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);++i)if(ioctl(fd,UI_SET_KEYBIT,keys[i])<0)fail("buttons");
    const int axes[]={ABS_X,ABS_Y,ABS_Z,ABS_RX,ABS_RY,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
    for(size_t i=0;i<sizeof(axes)/sizeof(*axes);++i){
        int hat=i>=6,trigger=i==2 || i==5;
        struct uinput_abs_setup a={.code=axes[i],.absinfo={.minimum=hat?-1:0,.maximum=hat?1:255,.value=hat||trigger?0:128}};
        if(ioctl(fd,UI_ABS_SETUP,&a)<0)fail("axes");
    }
    if(ioctl(fd,UI_DEV_SETUP,&u)<0 || ioctl(fd,UI_DEV_CREATE)<0)fail("create gamepad");
    puts("READY");
    struct pollfd p[2]={{STDIN_FILENO,POLLIN,0},{fd,POLLIN,0}};
    for(;;){
        if(poll(p,2,-1)<0){if(errno==EINTR)continue;fail("poll");}
        if(p[1].revents&POLLIN){
            struct input_event ev[16];ssize_t n=read(fd,ev,sizeof(ev));
            if(n<0 && errno!=EAGAIN)fail("read feedback");
            for(ssize_t i=0;i<n/(ssize_t)sizeof(*ev);++i)feedback(fd,ev[i]);
        }
        if(p[0].revents&(POLLIN|POLLHUP)){
            char line[512],*at=line;unsigned t,c;int v,n;
            if(!fgets(line,sizeof(line),stdin) || !strcmp(line,"quit\n"))break;
            while(sscanf(at,"%u %u %d%n",&t,&c,&v,&n)==3){emit(fd,t,c,v);at+=n;}
            emit(fd,EV_SYN,SYN_REPORT,0);puts("SENT");
        }
    }
    if(ioctl(fd,UI_DEV_DESTROY)<0)fail("destroy gamepad");
    close(fd);return 0;
}
