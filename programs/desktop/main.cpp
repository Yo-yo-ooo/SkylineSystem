#include <unisted.h>
#include <syscall.h>
#include <signal.h>

void new_op(int signum, siginfo_t *info, void *myact)
{
    //printf("receive signam %d\n", signum);
    ssize_t ret;
    syscall3(
        __NR_write,
        (uint64_t)1,
        (uint64_t)"receive signam\n",
        (uint64_t)16,
        ret
    );
}

//test exit in user prog

int main(){
    /* ssize_t x = write(1,"Hello, World!\n",13); */
    struct sigaction* act;
    int sig;
    int ret;
    ssize_t ret3;
    //act.sa_flags = SA_SIGINFO;
    act->sa_sigaction = new_op;
    syscall3(
        __NR_write,
        (uint64_t)1,
        (uint64_t)"OK THERE!\n",
        (uint64_t)11,
        ret
    );
    
    syscall3(__NR_rt_sigaction, 
        (uint64_t)sig, 
        (uint64_t)act, 
        (uint64_t)NULL,ret
    );
    if(ret < 0){
        ssize_t ret2;
        syscall3(
            __NR_write,
            (uint64_t)1,
            (uint64_t)"Install ERROR\n",
            (uint64_t)15,
            ret
        );
    }
    while(true);
    return 0;
}