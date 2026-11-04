/* Independent owner of the engine child restores Bluetooth on UI/broker loss.
 * No system daemon control, guessed PIDs, or elapsed-time session limit. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;
static void signal_stop(int sig){(void)sig;stop_requested=1;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int send_all(int fd,const char *bytes,size_t size) {
    while(size){ssize_t n=write(fd,bytes,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;bytes+=n;size-=(size_t)n;}return 0;
}
static void message(int fd,const char *text){send_all(fd,text,strlen(text));}
static int pipe_cloexec(int descriptors[2]) {
    if(pipe(descriptors))return -1;
    fcntl(descriptors[0],F_SETFD,FD_CLOEXEC);fcntl(descriptors[1],F_SETFD,FD_CLOEXEC);return 0;
}
static int power(const char *helper,const char *command) {
    int output[2];if(pipe_cloexec(output))return -1;
    pid_t child=fork();if(child<0){close(output[0]);close(output[1]);return -1;}
    if(child==0){dup2(output[1],STDOUT_FILENO);close(output[0]);close(output[1]);execl(helper,helper,command,NULL);_exit(127);}
    close(output[1]);fcntl(output[0],F_SETFL,O_NONBLOCK);
    char data[128]={0};size_t used=0;double end=now()+6;int status=0,finished=0;
    while(now()<end) {
        ssize_t n=read(output[0],data+used,sizeof(data)-1-used);if(n>0)used+=(size_t)n;
        if(waitpid(child,&status,WNOHANG)==child){finished=1;break;}
        struct pollfd p={.fd=output[0],.events=POLLIN};poll(&p,1,30);
    }
    if(!finished){kill(child,SIGKILL);waitpid(child,&status,0);close(output[0]);fprintf(stderr,"POWER_HELPER_FAILED command=%s timeout=1\n",command);return -1;}
    ssize_t n=read(output[0],data+used,sizeof(data)-1-used);if(n>0)used+=(size_t)n;data[used]=0;close(output[0]);
    if(!WIFEXITED(status)||WEXITSTATUS(status)){fprintf(stderr,"POWER_HELPER_FAILED command=%s wait_status=%d\n",command,status);return -1;}
    if(!strcmp(command,"state")){
        int value=data[0]=='0'?0:data[0]=='1'?1:-1;
        if(value<0)fprintf(stderr,"POWER_HELPER_FAILED command=state invalid_response=1 bytes=%zu\n",used);
        return value;
    }
    return 0;
}
static void terminate_child(pid_t child) {
    int status;
    if(waitpid(child,&status,WNOHANG)==child)return;
    kill(child,SIGTERM);double end=now()+2;
    while(now()<end){if(waitpid(child,&status,WNOHANG)==child)return;struct timespec t={0,20000000};nanosleep(&t,NULL);}
    kill(child,SIGKILL);waitpid(child,&status,0);
}

static void guardian(const char *engine,const char *helper,const char *directory,int original,
                     int lease,int input,int output,int events,int lock) {
    setsid();signal(SIGTERM,SIG_IGN);signal(SIGINT,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    int null=open("/dev/null",O_RDWR);for(int i=0;i<3;i++)dup2(null,i);if(null>2)close(null);
    message(events,"{\"event\":\"starting\"}\n");
    pid_t child=-1;
    if(original==1&&power(helper,"off")) {
        message(events,"{\"event\":\"error\",\"message\":\"系统未确认关闭蓝牙，接收未启动\"}\n");
    } else {
        child=fork();
        if(child==0) {
            signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
            dup2(input,STDIN_FILENO);dup2(output,STDOUT_FILENO);dup2(output,STDERR_FILENO);
            close(input);close(output);close(lease);close(events);close(lock);
            execl(engine,engine,"--run","--state-dir",directory,"--session-token","menu-session",NULL);_exit(127);
        }
        if(child<0)message(events,"{\"event\":\"error\",\"message\":\"无法启动音频接收程序\"}\n");
    }
    close(input);close(output);
    double last=now();int requested=0,status=0;
    while(child>0) {
        if(waitpid(child,&status,WNOHANG)==child){child=-1;break;}
        struct pollfd p={.fd=lease,.events=POLLIN|POLLHUP};int r=poll(&p,1,200);
        if(r>0) {
            char buffer[128];ssize_t count=read(lease,buffer,sizeof(buffer));
            if(count<=0){requested=1;break;}
            for(ssize_t i=0;i<count;i++)if(buffer[i]=='S')requested=1;
            last=now();if(requested)break;
        }
        if(now()-last>12){requested=1;break;}
    }
    if(child>0)terminate_child(child);
    if(!requested && WIFEXITED(status) && WEXITSTATUS(status))
        message(events,"{\"event\":\"notice\",\"message\":\"接收程序已退出，系统蓝牙正在恢复\"}\n");
    int restored=power(helper,original?"on":"off")==0;
    message(events,restored?"{\"event\":\"stopped\",\"restored\":true}\n":
        "{\"event\":\"error\",\"message\":\"系统尚未确认恢复蓝牙，请在系统设置中检查\"}\n");
    close(lease);close(events);close(lock);_exit(restored?0:4);
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);umask(0077);
    /* Reap our own helpers even if a caller ignores child notifications. */
    signal(SIGCHLD,SIG_DFL);
    if(argc!=4||strcmp(argv[1],"--run")||getuid()==0)return 64;
    char engine[PATH_MAX],helper[PATH_MAX],lockname[PATH_MAX];
    if(strlen(argv[2])>PATH_MAX-40||strlen(argv[3])>PATH_MAX-40)return 64;
    snprintf(engine,sizeof(engine),"%s/bluetooth-mixer-engine",argv[2]);
    snprintf(helper,sizeof(helper),"%s/bt-power",argv[2]);snprintf(lockname,sizeof(lockname),"%s/session.lock",argv[3]);
    if(mkdir(argv[3],0700)&&errno!=EEXIST)return 2;
    struct stat st;if(lstat(argv[3],&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid())return 2;
    chmod(argv[3],0700);
    int lock=open(lockname,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) {
        puts("{\"event\":\"error\",\"message\":\"已有接收会话正在运行或恢复\"}");return 2;
    }
    fcntl(lock,F_SETFD,FD_CLOEXEC);
    int original=power(helper,"state");if(original<0){puts("{\"event\":\"error\",\"message\":\"无法读取系统蓝牙状态，请检查蓝牙权限或诊断日志\"}");close(lock);return 3;}
    int lease[2],input[2],output[2],events[2];
    if(pipe_cloexec(lease)||pipe_cloexec(input)||pipe_cloexec(output)||pipe_cloexec(events))return 3;
    signal(SIGPIPE,SIG_IGN);signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);
    pid_t owner=fork();
    if(owner<0)return 3;
    if(owner==0) {
        close(lease[1]);close(input[1]);close(output[0]);close(events[0]);
        guardian(engine,helper,argv[3],original,lease[0],input[0],output[1],events[1],lock);
    }
    close(lease[0]);close(input[0]);close(output[1]);close(events[1]);
    double heartbeat=0;int shutdown=0,status=0,owner_done=0,output_open=1,events_open=1;
    char command[4096];size_t command_size=0;
    char lines[2][16384];size_t line_sizes[2]={0,0};
    while(!owner_done||output_open||events_open) {
        if(!shutdown&&stop_requested){shutdown=1;message(lease[1],"S");close(input[1]);input[1]=-1;}
        if(now()-heartbeat>1) {
            message(lease[1],"H");if(input[1]>=0)message(input[1],"PING\n");heartbeat=now();
        }
        struct pollfd fds[3]={{.fd=shutdown?-1:STDIN_FILENO,.events=POLLIN|POLLHUP},
            {.fd=output_open?output[0]:-1,.events=POLLIN|POLLHUP},
            {.fd=events_open?events[0]:-1,.events=POLLIN|POLLHUP}};
        poll(fds,3,100);
        if(fds[0].revents) {
            char buffer[1024];ssize_t n=read(STDIN_FILENO,buffer,sizeof(buffer));
            if(n<=0)stop_requested=1;
            else for(ssize_t i=0;i<n;i++) {
                if(buffer[i]=='\n') {
                    command[command_size]=0;
                    if(!strcmp(command,"STOP"))stop_requested=1;
                    else if(input[1]>=0){message(input[1],command);message(input[1],"\n");}
                    command_size=0;
                }else if(command_size<sizeof(command)-1)command[command_size++]=buffer[i];
            }
        }
        for(int i=1;i<3;i++)if(fds[i].revents) {
            char buffer[4096];ssize_t n=read(fds[i].fd,buffer,sizeof(buffer));
            if(n>0) {
                for(ssize_t j=0;j<n;j++) {
                    size_t *used=&line_sizes[i-1];
                    if(*used<sizeof(lines[0])-1)lines[i-1][(*used)++]=buffer[j];
                    if(buffer[j]=='\n') {
                        if(send_all(STDOUT_FILENO,lines[i-1],*used))stop_requested=1;
                        *used=0;
                    }
                }
            }
            else if(i==1)output_open=0;else events_open=0;
        }
        if(!owner_done&&waitpid(owner,&status,WNOHANG)==owner)owner_done=1;
        if(owner_done&&!events_open&&(!WIFEXITED(status)||WEXITSTATUS(status)!=0)) {
            if(input[1]>=0){close(input[1]);input[1]=-1;}
            power(helper,original?"on":"off");
        }
    }
    if(input[1]>=0)close(input[1]);close(lease[1]);close(output[0]);close(events[0]);close(lock);
    return WIFEXITED(status)?WEXITSTATUS(status):3;
}
