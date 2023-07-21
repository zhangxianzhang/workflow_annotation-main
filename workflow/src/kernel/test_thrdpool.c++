#include "thrdpool.h"
#include <cstdio>

void my_routine(void *context)                                                   
{
   // 我们要执行的函数
   thrdpool_t *pool = (thrdpool_t *)context;        
   printf("%p \n", context) ;            
    
}                                                                               



void my_pending(const struct thrdpool_task *task) 
{
  // 线程池销毁后，没执行的任务会到这里
    printf("pending task-%llu.\n", reinterpret_cast<unsigned long long>(task->context));                                    
} 

void my_routine_1(void *thrd_pool)                                                   
{
   // 我们要执行的函数
   thrdpool_t *pool = (thrdpool_t *)thrd_pool;                    
    thrdpool_destroy(&my_pending, pool); // 结束
}    

int main()                                                                         
{
    thrdpool_t *thrd_pool = thrdpool_create(3, 1024); // 创建                            
    struct thrdpool_task task;
    unsigned long long i;
                               
    for (i = 0; i < 5; i++)
    {
        task.routine = &my_routine;                                             
        task.context = reinterpret_cast<void *>(thrd_pool);  
        if( i == 4)
        {
            task.routine = &my_routine_1;                                             
            task.context = thrd_pool;   
        }                           
        thrdpool_schedule(&task, thrd_pool); // 调用
    }
    getchar(); // 卡住主线程，按回车继续
    printf("out %p \n", thrd_pool) ;
    thrdpool_destroy(&my_pending, thrd_pool); // 结束
    return 0;                                                                   
} 