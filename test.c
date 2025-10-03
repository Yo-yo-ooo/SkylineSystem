int main(){
    asm volatile("movq  $1, %rax\n\t"
                 "syscall"
                 );
    while(1);
}
