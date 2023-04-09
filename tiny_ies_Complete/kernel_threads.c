
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"


/** 
  @brief Create a new thread in the current process.
  */

Tid_t sys_CreateThread(Task task, int argl, void* args)
{

  PCB* curr_proc = CURPROC;
  PTCB* curptcb;
  curptcb = acquire_PTCB();

  curptcb->parent = curr_proc;
  curptcb->task = task;
  curptcb->argl = argl;
  curptcb->args = args;
  curptcb->join_var = COND_INIT;
  curptcb->refcount = 0;
  curptcb->flag_detached = 0;
  curptcb->flag_exited = 0;

  curptcb->tcb_thread = spawn_thread(curr_proc, start_thread);  //spawn a new thread for the thread list pf process
  curptcb->tcb_thread->owner_ptcb = curptcb;
  rlnode_init(&curptcb->thread_node, curptcb);
  rlist_push_back(&curr_proc->ptcb_list, &curptcb->thread_node);

  Tid_t tid = (Tid_t)curptcb->tcb_thread;

  curr_proc->ptcb_count++;
  curr_proc->active_count++;
  
  
  wakeup(curptcb->tcb_thread);
  return tid;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
  return (Tid_t) CURTHREAD;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  PTCB* cur_ptcb = NULL;
  //PCB* curr_proc = CURPROC;
  //TCB* cur_thr = CURTHREAD;
  rlnode* tmp;

  int list_length = rlist_len(&CURPROC->ptcb_list);

  if(list_length!=0) {
      for (int i = 0; i < list_length; ++i) {
          tmp = rlist_pop_front(&CURPROC->ptcb_list);
          if (tmp != NULL && tmp->ptcb != NULL) {
              if (tmp->ptcb->tcb_thread == (TCB *) tid) {
                  cur_ptcb = tmp->ptcb;
              }
              rlist_push_back(&CURPROC->ptcb_list, &tmp->ptcb->thread_node);
          }
      }
      if (cur_ptcb != NULL || tid != (Tid_t) CURTHREAD || !cur_ptcb->flag_detached) {
          cur_ptcb->refcount++;
          while (!cur_ptcb->flag_exited && !cur_ptcb->flag_detached) {
              kernel_wait(&cur_ptcb->join_var, SCHED_USER);
          }
          if (exitval != NULL) {
              *exitval = cur_ptcb->exitval;
              cur_ptcb->refcount--;
          }

          if (cur_ptcb->refcount == 0) {
              rlist_remove(&cur_ptcb->thread_node);
              release_PTCB(cur_ptcb);
          }
          return 0;
      } else {
          return -1;
      }
  }
  else{
          return -1;
      }
}


int sys_ThreadDetach(Tid_t tid)
{
  //PCB* current_proc = CURPROC;
    rlnode* tmp;
  PTCB* cur_ptcb = NULL;

  int list_length = rlist_len(&CURPROC->ptcb_list);

  if(list_length!=0) {
      for (int i = 0; i < list_length; ++i) {
          tmp = rlist_pop_front(&CURPROC->ptcb_list);
          if (tmp != NULL && tmp->ptcb != NULL) {
              if (tmp->ptcb->tcb_thread == (TCB *) tid) {
                  cur_ptcb = tmp->ptcb;
              }
              rlist_push_back(&CURPROC->ptcb_list, &tmp->ptcb->thread_node);
          }
      }
      if (cur_ptcb != NULL && tid == (Tid_t) CURTHREAD && cur_ptcb->tcb_thread->state!=EXITED) {
          
          cur_ptcb->flag_detached = 1;
          return 0;
      }
      else{
        return -1;
      } 
      
  }
  else{
    return -1;
  }
}



/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  PCB* curr_proc = CURPROC;
  TCB* cur_thr = CURTHREAD;
  PTCB* cur_ptcb = cur_thr->owner_ptcb;

  if(curr_proc->active_count > 0){
    kernel_broadcast(& cur_ptcb->join_var); // we wake up all the threads that joined to this thread 
                                          // so no one is waiting
    cur_ptcb->flag_exited = 1;
    cur_ptcb->exitval = exitval;

    // if detached then remove
    if(cur_ptcb->flag_detached)
    {
      rlist_remove(&cur_ptcb->thread_node); 
      release_PTCB(cur_ptcb);               
    }

    curr_proc->active_count--;        //existing threads -1
  }
  //if the current thread is the main,then we must call sysExit to terminate it
  if(curr_proc->active_count == 0)
    sys_Exit(exitval);
  else
    kernel_sleep(EXITED, SCHED_USER);   //zzz
}

