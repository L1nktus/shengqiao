#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

static int output(NSDictionary *object) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:nil];
    fwrite(data.bytes,1,data.length,stdout);fputc('\n',stdout);
    return [object[@"ok"] boolValue]?0:1;
}
static int failure(NSString *code,NSString *message) {
    return output(@{@"ok":@NO,@"code":code,@"message":message});
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void pause_short(void){struct timespec t={0,100000000};nanosleep(&t,NULL);}
static int connect_control(NSString *path) {
    struct sockaddr_un address={0};address.sun_len=sizeof(address);address.sun_family=AF_UNIX;
    const char *bytes=path.fileSystemRepresentation;
    if(strlen(bytes)>=sizeof(address.sun_path)){errno=ENAMETOOLONG;return -1;}
    strcpy(address.sun_path,bytes);
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return -1;}
    uid_t uid;gid_t gid;
    if(getpeereid(fd,&uid,&gid)||uid!=getuid()){close(fd);errno=EACCES;return -1;}
    struct timeval timeout={35,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    timeout.tv_sec=2;setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    return fd;
}
static int session_free(NSString *directory) {
    int fd=open([[directory stringByAppendingPathComponent:@"session.lock"] fileSystemRepresentation],O_RDWR|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT;
    int free=flock(fd,LOCK_EX|LOCK_NB)==0;close(fd);return free;
}
static int launch_app(void) {
    struct stat console;if(stat("/dev/console",&console)||console.st_uid!=getuid())return -1;
    char binary[PATH_MAX];uint32_t size=sizeof(binary);
    if(_NSGetExecutablePath(binary,&size))return -1;
    char resolved[PATH_MAX];if(!realpath(binary,resolved))return -1;
    for(int i=0;i<3;i++){char *slash=strrchr(resolved,'/');if(!slash)return -1;*slash=0;}
    pid_t child=fork();if(child<0)return -1;
    if(!child){execl("/usr/bin/open","open","-n","-g",resolved,NULL);_exit(127);}
    double deadline=now()+8;int status;
    while(now()<deadline){
        if(waitpid(child,&status,WNOHANG)==child)return WIFEXITED(status)&&!WEXITSTATUS(status)?0:-1;
        pause_short();
    }
    kill(child,SIGTERM);waitpid(child,&status,0);return -1;
}
int main(int argc,char **argv){@autoreleasepool{
    signal(SIGPIPE,SIG_IGN);
    signal(SIGCHLD,SIG_DFL);
    if(argc!=2){fprintf(stderr,"Usage: btmixctl on|off|toggle|status\n");return 64;}
    NSString *command=@(argv[1]);
    if([command isEqualToString:@"--help"]||[command isEqualToString:@"-h"]){puts("Usage: btmixctl on|off|toggle|status\nUse the Mac login user over SSH; prints JSON and waits for completion.");return 0;}
    if(![@[@"on",@"off",@"toggle",@"status"] containsObject:command])return failure(@"invalid_command",@"仅支持 on、off、toggle、status");
    if(getuid()==0)return failure(@"wrong_user",@"请用 Mac 桌面的登录用户运行，不要使用 sudo");
    NSString *directory=[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/BluetoothMixer"];
    NSString *path=[directory stringByAppendingPathComponent:@"control.sock"];
#ifdef BTMIX_TEST_SOCKET
    const char *test=getenv("BTMIX_TEST_SOCKET");if(test)path=@(test);
#endif
    int fd=connect_control(path);
    if(fd<0){
#ifdef BTMIX_TEST_SOCKET
        return failure(@"not_running",@"Test host is not running");
#else
        double deadline=now()+25;
        if([command isEqualToString:@"status"]&&!session_free(directory))return failure(@"recovering",@"接收程序正在恢复系统蓝牙，请稍后查询");
        while(!session_free(directory)&&now()<deadline)pause_short();
        if(!session_free(directory))return failure(@"recovering",@"系统蓝牙仍在恢复，请稍后重试");
        if([command isEqualToString:@"off"]||[command isEqualToString:@"status"])
            return output(@{@"ok":@YES,@"state":@"off",@"receiving":@NO,@"requested":@NO,@"running":@NO,@"ready":@NO});
        if(launch_app())return failure(@"launch_failed",@"无法启动声桥，请先在 Mac 登录桌面，并使用同一用户连接 SSH");
        deadline=now()+10;
        do{fd=connect_control(path);if(fd>=0)break;pause_short();}while(now()<deadline);
        if(fd<0)return failure(@"control_unavailable",@"声桥控制接口未就绪，请检查应用版本和诊断日志");
#endif
    }
    NSData *request=[[command stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
    const char *bytes=request.bytes;size_t sent=0;
    while(sent<request.length){ssize_t n=write(fd,bytes+sent,request.length-sent);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return failure(@"disconnected",@"控制连接已断开");}sent+=(size_t)n;}
    NSMutableData *response=[NSMutableData data];char buffer[1024];
    while(response.length<8192){
        ssize_t n=read(fd,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;
        if(n<=0)break;[response appendBytes:buffer length:(NSUInteger)n];if(memchr(buffer,'\n',(size_t)n))break;
    }
    close(fd);
    NSDictionary *reply=[NSJSONSerialization JSONObjectWithData:response options:0 error:nil];
    if(![reply isKindOfClass:[NSDictionary class]]||!reply[@"ok"])return failure(@"no_response",@"未收到操作结果，应用可能正在退出或未响应");
    return output(reply);
}}
