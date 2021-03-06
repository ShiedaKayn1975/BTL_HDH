#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "pstat.h"

struct {
    struct spinlock lock;      // xử lý race condition
    struct proc proc[NPROC]; //Mang tien trinh
} ptable;

static struct proc *initproc;

static const int DEBUG = 0;

int maxTicks[4] = { 0, 32, 16, 8 };       // Maximun Ticks at each level  
int starvationMax[3] = { 500, 320, 160 }; // Starvation ticks at each level
struct proc *PQ[4][NPROC];                // to store processes at each priority NPROC: so tien trinh toi da 1 quecue là 64

// init all processes at ech priority as NULL Khởi tạo tất cả process ở mỗi queue với giá trị NULL
void initPQ() {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < NPROC; j++) {
            PQ[i][j] = NULL;
        }
    }
}

// utility to print PQ for debugging
void printPQ() {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < NPROC; j++) {
            if (PQ[i][j]) {
                cprintf("Pri:%d, pid:%d\n", i, PQ[i][j]->pid);
            }
        }
    }
}

// insert process iproc at priority pri in PQ
void addToPQ(int pri, struct proc *iproc) { //truyen 1 process *iproc, pri là priority là vị trí của Queue trong mảng
    //cprintf("Add process:%d pri:%d\n",iproc->pid,pri);
    if (DEBUG) cprintf("****************ADD BLOCK ******************\n");
    if (DEBUG) printPQ();
    if (DEBUG) cprintf("After print\n");

    int i = 0;
    while (i < NPROC) {
        if (PQ[pri][i] == NULL) break; //Kiem tra queue , nếu gặp index có process chưa sử dụng thì dừng lại, lưu giá trị của vị trí trong mảng vào i, còn nếu ko thì i++ tiếp tục duyệt
        i++;
    }

    PQ[pri][i] = iproc; // gán process iproc cho process ở queue pri có vị trí i
    if (DEBUG) printPQ();
    if (DEBUG) cprintf("=============================================\n");
}

// delete process iproc at priority pri
void deleteFromPQ(int pri, struct proc *iproc) {   //hàm này xóa process từ PQ , truyền vào pri( priority) và process
    if (DEBUG) cprintf("**************** DELETE BLOCK ********************\n");
    if (DEBUG) printPQ();
    if (DEBUG) cprintf("deleting pri:%d pid:%d\n", pri, iproc->pid);

    int i = 0;   // index
    while (PQ[pri][i] != iproc && i < NPROC) {  // duyệt đến khi nào gặp process có giá trị bằng với iproc truyền vào hàm và đảm bảo cho i ko vượt quá 64 là NPROC
        i++;
    }
    // hết đoạn này lấy đc giá trị i
    if ((i == NPROC - i) || (PQ[pri][i + 1] == NULL)) {
        PQ[pri][i] = NULL;
        if (DEBUG) printPQ();
        if (DEBUG) cprintf("=============================================\n");
        return;
    }
    while (PQ[pri][i + 1]) {
        PQ[pri][i]     = PQ[pri][i + 1];
        PQ[pri][i + 1] = NULL;
        ++i;
    }
    if (DEBUG) printPQ();
    if (DEBUG) cprintf("=============================================\n");
}

// update waitticks of all runnable processes other then iproc
void updateTicks(struct proc *iproc) {
    struct proc *p;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == RUNNABLE) { //Tien trinh co kha nang chay
            if ((p->pid != iproc->pid) && (p->priority <= iproc->priority)) {//Tang thoi gian doi cac tien trinh
                ++p->ticksWait;
                ++p->wait_ticks[p->priority];
            }
        }
    }
}

// remove completed processes from PQ and
// increase priority of starved processes
void boostPQ() {
    for (int i = 0; i < 4; i++) {
        int j = 0;
        while (j < NPROC) {
            if (PQ[i][j] && (PQ[i][j]->state == UNUSED)) {//Tien trinh khac NULL va o trang thai UNUSED ()
                // Process complete. Delete from PQ.
                deleteFromPQ(i, PQ[i][j]);
            } else if (PQ[i][j]) {
                if ((i != 3) && (PQ[i][j]->ticksWait >= starvationMax[i])) {
                    if (DEBUG) printPQ();
                    if (DEBUG) cprintf(
                            "Boosting pri:%d, pid: %d wait:%d, maxStarv%d\n",
                            i,
                            PQ[i][j]->pid,
                            PQ[i][j]->ticksWait,
                            starvationMax[i]);
                    struct proc *tempProc = PQ[i][j];
                    tempProc->ticksWait    = 0;
                    tempProc->ticksCurrent = 0;
                    ++tempProc->priority;
                    deleteFromPQ(i, tempProc);
                    addToPQ(i + 1, tempProc);
                    if (DEBUG) printPQ();
                } else {
                    j++;
                }
            } else {
                j++;
            }
        }
    }
}

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
            pinit(void) {
    initlock(&ptable.lock, "ptable");
}

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *
allocproc(void) {
    struct proc *p;
    char *sp;

    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == UNUSED) goto found;
    release(&ptable.lock);
    return 0;

found:
    p->state = EMBRYO;
    p->pid   = nextpid++;
    release(&ptable.lock);

    // Allocate kernel stack if possible.
    if ((p->kstack = kalloc()) == 0) {
        p->state = UNUSED;
        return 0;
    }
    sp = p->kstack + KSTACKSIZE;

    // Leave room for trap frame.
    sp   -= sizeof *p->tf;
    p->tf = (struct trapframe *)sp;

    // Set up new context to start executing at forkret,
    // which returns to trapret.
    sp         -= 4;//4=constant
    *(uint *)sp = (uint)trapret;

    sp        -= sizeof *p->context;
    p->context = (struct context *)sp;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (uint)forkret;

    // Set current priority  as highest (3)
    p->priority     = 3;//SOQUECE
    p->ticksCurrent = 0;
    p->ticksWait    = 0;
    for (int i = 0; i < 4; i++) {//Thoi gian o tat ca cac queue =0
        p->ticks[i]      = 0;
        p->wait_ticks[i] = 0;
    }
    addToPQ(3, p);
    return p;
}

// Set up first user process.
void
userinit(void) {
    struct proc *p;
    extern char  _binary_initcode_start[], _binary_initcode_size[];

    initPQ();
    p = allocproc();
    acquire(&ptable.lock);
    initproc = p;
    if ((p->pgdir = setupkvm()) == 0) panic("userinit: out of memory?");
    inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
    p->sz = PGSIZE;
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->cs     = (SEG_UCODE << 3) | DPL_USER;
    p->tf->ds     = (SEG_UDATA << 3) | DPL_USER;
    p->tf->es     = p->tf->ds;
    p->tf->ss     = p->tf->ds;
    p->tf->eflags = FL_IF;
    p->tf->esp    = PGSIZE;
    p->tf->eip    = 0; // beginning of initcode.S

    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    p->state = RUNNABLE;
    release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n) {
    uint sz;

    sz = proc->sz;
    if (n > 0) {
        if ((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0) return -1;
    } else if (n < 0) {
        if ((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0) return -1;
    }
    proc->sz = sz;
    switchuvm(proc);
    return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void) {
    int i, pid;
    struct proc *np;

    // Allocate process.
    if ((np = allocproc()) == 0) return -1;

    // Copy process state from p.
    if ((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0) {
        kfree(np->kstack);
        np->kstack = 0;
        np->state  = UNUSED;
        return -1;
    }
    np->sz     = proc->sz;
    np->parent = proc;
    *np->tf    = *proc->tf;

    // Clear %eax so that fork returns 0 in the child.
    np->tf->eax = 0;

    for (i = 0; i < NOFILE; i++)
        if (proc->ofile[i]) np->ofile[i] = filedup(proc->ofile[i]);
    np->cwd = idup(proc->cwd);

    pid       = np->pid;
    np->state = RUNNABLE;
    safestrcpy(np->name, proc->name, sizeof(proc->name));
    return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void) {
    struct proc *p;
    int fd;

    if (proc == initproc) panic("init exiting");

    // Close all open files.
    for (fd = 0; fd < NOFILE; fd++) {
        if (proc->ofile[fd]) {
            fileclose(proc->ofile[fd]);
            proc->ofile[fd] = 0;
        }
    }

    iput(proc->cwd);
    proc->cwd = 0;

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(proc->parent);

    // Pass abandoned children to init.
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->parent == proc) {
            p->parent = initproc;
            if (p->state == ZOMBIE) wakeup1(initproc);
        }
    }

    // Jump into the scheduler, never to return.
    proc->state = ZOMBIE;
    sched();

    // procdump();
    // printPQ();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void) {
    struct proc *p;
    int havekids, pid;

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for zombie children.
        havekids = 0;
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != proc) continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);
                p->state   = UNUSED;
                p->pid     = 0;
                p->parent  = 0;
                p->name[0] = 0;
                p->killed  = 0;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || proc->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(proc, &ptable.lock); // DOC: wait-sleep
    }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void) {
    struct proc *p;

    for (;;) {
        // Enable interrupts on this processor.
        sti();
        acquire(&ptable.lock);

        // Loop over process table looking for process to run.
        boostPQ(); // Boost priorities
        int toBreak = 0;
        for (int pr = 3; pr >= 0; pr--) {//Xac dinh ton tai tien trinh o trang thai RUNNABLE
            for (int i = 0; i < NPROC; i++) {
                if (PQ[pr][i] && (PQ[pr][i]->state == RUNNABLE)) {
                    p       = PQ[pr][i];
                    toBreak = 1;
                    break;
                }
            }
            if (toBreak == 1) {
                break;
            }
        }
        if (toBreak == 0) {
            // No runnable process found. continue to loop
            release(&ptable.lock);
            continue;
        }

        // switch to process
        proc = p;

        p->ticksWait    = 0;
        ++p->ticksCurrent;                // increase ticks used in current//Tong thoi gian tich luy o tat cac queue
                                          // priority
        ++p->ticks[p->priority];          // increase total ticks used in
                                          // current
                                          // priority
        if ((p->priority != 0) && (p->ticksCurrent >= maxTicks[p->priority])) {//Day xuong quece duoi
            // reduce priority
            deleteFromPQ(p->priority, p); // delete from current priority
            --p->priority;                // reduce priority
            addToPQ(p->priority, p);      // add to new priority queue
            p->ticksCurrent = 0;          // set ticks used in new priority 0
            p->ticksWait    = 0;          // set wait ticks in new priority 0
        }

        // schedule process
        switchuvm(p);
        p->state = RUNNING;
        swtch(&cpu->scheduler, proc->context);
        switchkvm();

        // Returs here from scheduler
        updateTicks(p);

        proc = 0;
        release(&ptable.lock);
    }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void) {
    int intena;

    if (!holding(&ptable.lock)) panic("sched ptable.lock");
    if (cpu->ncli != 1) panic("sched locks");
    if (proc->state == RUNNING) panic("sched running");
    if (readeflags() & FL_IF) panic("sched interruptible");
    intena = cpu->intena;
    swtch(&proc->context, cpu->scheduler);
    cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void) {
    acquire(&ptable.lock); // DOC: yieldlock
    proc->state = RUNNABLE;
    sched();
    release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void) {
    // Still holding ptable.lock from scheduler.
    release(&ptable.lock);

    // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk) {
    if (proc == 0) panic("sleep");

    if (lk == 0) panic("sleep without lk");

    // Must acquire ptable.lock in order to
    // change p->state and then call sched.
    // Once we hold ptable.lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup runs with ptable.lock locked),
    // so it's okay to release lk.
    if (lk != &ptable.lock) {  // DOC: sleeplock0
        acquire(&ptable.lock); // DOC: sleeplock1
        release(lk);
    }

    // Go to sleep.
    proc->chan  = chan;
    proc->state = SLEEPING;
    deleteFromPQ(proc->priority,proc);
    sched();

    // Tidy up.
    proc->chan = 0;

    // Reacquire original lock.
    if (lk != &ptable.lock) { // DOC: sleeplock2
        release(&ptable.lock);
        acquire(lk);
    }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan) {
    struct proc *p;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        if ((p->state == SLEEPING) && (p->chan == chan))
        {
            p->state = RUNNABLE;
            addToPQ(p->priority,p);
        }
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan) {
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid) {
    struct proc *p;
    cprintf("Kill called: %d\n", pid);
    procdump();
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->pid == pid) {
            p->killed = 1;

            // Wake process from sleep if necessary.
            if (p->state == SLEEPING)
            {
                 p->state = RUNNABLE;
                 addToPQ(p->priority,p);
            }
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getpinfo(struct pstat *p_stat) {
    //procdump();
    //printPQ();
    for (int i = 0; i < NPROC; i++) {
        struct proc *curr = &ptable.proc[i];
        if (curr->state == UNUSED) p_stat->inuse[i] = 0;
        else {
            p_stat->inuse[i]    = 1;
            p_stat->pid[i]      = curr->pid;
            p_stat->priority[i] = curr->priority;
            p_stat->state[i]    = curr->state;
            if (DEBUG) cprintf(
                    "Pid:%d pri:%d ticks: %d %d %d %d wait:%d %d %d %d\n",
                    curr->pid,
                    curr->priority,
                    curr->ticks[0],
                    curr->ticks[1],
                    curr->ticks[2],
                    curr->ticks[3],
                    curr->wait_ticks[0],
                    curr->wait_ticks[1],
                    curr->wait_ticks[2],
                    curr->wait_ticks[3]);
            for (int z = 0; z <= 4; z++) {
                p_stat->ticks[i][z]      = curr->ticks[z];
                p_stat->wait_ticks[i][z] = curr->wait_ticks[z];
            }
        }
    }
    return 0;
}
