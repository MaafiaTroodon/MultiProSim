#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROCS  100
#define MAX_NODES  100
#define MAX_OPS    256

// Process life cycle flags used by run loop and logs
typedef enum { NEW, READY, RUNNING, BLOCKED, FINISHED } State;
// Operation kinds read from input and executed by runner
typedef enum { DOOP, BLOCK, HALT, SEND, RECV, INVALID } OpType;


// One instruction in program stream
typedef struct {
    OpType type;
    int a;              // DOOP or BLOCK ticks, SEND or RECV address as node times one hundred plus pid
} Operation;

// Control block for one process
typedef struct Process {
    // static info
    char name[32];
    int size, priority, node;   // node ids start at one
    int pid_global;             // one based id across all procs
    int node_pid;               // one based id within node

    // program
    Operation ops[MAX_OPS];
    int op_count, pc;

    // dynamic
    State state;
    int run_time, block_time, wait_time, finish_time;
    int unblock_time;   // absolute time on this node for a timed BLOCK

    int sends, recvs;

    // rendezvous wish kept while BLOCKED on SEND or RECV
    // sender sets want_dst_addr
    // receiver sets want_src_addr
    int want_dst_addr;
    int want_src_addr;
} Process;

// Deferred state change for a process on a node
typedef struct Pending {
    Process *p;
    int due_time;
    int is_finish;  // one means finish at due_time, zero means go READY at due_time
} Pending;

// One compute node with own clock and queues
typedef struct Node {
    int node_id;
    int quantum;
    int clock;

    Process *procs[MAX_PROCS];
    int proc_count;

    Process *ready[MAX_PROCS];   int ready_count;
    Process *blocked[MAX_PROCS]; int blocked_count;

    Pending pend[MAX_PROCS * 2]; int pend_count;
} Node;

/* --------- globals --------- */
// Shared store for all procs and nodes
static int total_procs, quantum, num_nodes;
static Process all_procs[MAX_PROCS];
static Node nodes[MAX_NODES + 1]; // nodes are one based

// List of SEND or RECV blocked procs for cross node match search
static Process *glob_blocked[MAX_PROCS * MAX_NODES];
static int glob_blocked_count = 0;

/* --------- helpers --------- */
// Map token text to an opcode
static OpType parse_op(const char *s) {
    if (strcmp(s, "DOOP") == 0)  return DOOP;
    if (strcmp(s, "BLOCK") == 0) return BLOCK;
    if (strcmp(s, "SEND")  == 0) return SEND;
    if (strcmp(s, "RECV")  == 0) return RECV;
    if (strcmp(s, "HALT")  == 0) return HALT;
    return INVALID;  // unknown token is not a HALT
}

// Read program with LOOP blocks expanded
// stop_on_end controls return when END appears inside body
static int parse_block_into(Operation *out, int *outc, int stop_on_end) {
    char tok[16];
    while (scanf("%15s", tok) == 1) {
        if (strcmp(tok, "END") == 0) {
            if (stop_on_end) return 0;   // end of a LOOP body
            continue;
        }
        if (strcmp(tok, "LOOP") == 0) {
            int times = 0;
            if (scanf("%d", &times) != 1) times = 0;

            Operation tmp[MAX_OPS];
            int tc = 0;
            parse_block_into(tmp, &tc, 1);   // read until END

            for (int r = 0; r < times; ++r) {
                for (int i = 0; i < tc; ++i) out[(*outc)++] = tmp[i];
            }
            continue;
        }

        OpType t = parse_op(tok);
        if (t == HALT) {
            out[*outc] = (Operation){ .type = HALT, .a = 0 };
            (*outc)++;
            return 1;  // program ends
        }
        if (t == DOOP || t == BLOCK || t == SEND || t == RECV) {
            int arg = 0; (void)scanf("%d", &arg);
            out[*outc] = (Operation){ .type = t, .a = arg };
            (*outc)++;
            continue;
        }

        // ignore other text such as labels or comments
    }
    return 0;
}


// Print one state change line in required format
static void print_state(int node_id, int time, int node_pid, const char *state) {
    printf("[%02d] %05d: process %d %s\n", node_id, time, node_pid, state);
}

// Check if next instruction is HALT
static int next_is_halt(Process *p) {
    return (p->pc < p->op_count && p->ops[p->pc].type == HALT);
}

// Address helpers for SEND and RECV
static int addr_node(int addr) { return addr / 100; }
static int addr_pid (int addr) { return addr % 100; }
static int proc_addr(Process *p) { return p->node * 100 + p->node_pid; }

/* READY / BLOCKED / PENDING management */
// Put proc into READY queue and log state
static void add_ready(Node *nd, Process *p) {
    p->state = READY;
    print_state(nd->node_id, nd->clock, p->node_pid, "ready");
    nd->ready[nd->ready_count++] = p;
}

// Append to BLOCKED list on this node
static void add_blocked(Node *nd, Process *p) {
    nd->blocked[nd->blocked_count++] = p;
}

// Remove one entry from BLOCKED list on this node
static void remove_blocked(Node *nd, Process *p) {
    for (int i = 0; i < nd->blocked_count; ++i) {
        if (nd->blocked[i] == p) {
            for (int j = i; j < nd->blocked_count - 1; ++j)
                nd->blocked[j] = nd->blocked[j + 1];
            nd->blocked_count--;
            return;
        }
    }
}

// Add a pending release or finish for time based events
static void add_pending(Node *nd, Process *p, int due_time, int is_finish) {
    nd->pend[nd->pend_count].p = p;
    nd->pend[nd->pend_count].due_time = due_time;
    nd->pend[nd->pend_count].is_finish = is_finish;
    nd->pend_count++;
}

// Spread wait time across ready set for dt ticks
static void add_wait_ready(Node *nd, int dt) {
    if (dt <= 0) return;
    for (int i = 0; i < nd->ready_count; ++i) {
        nd->ready[i]->wait_time += dt;
    }
}

/* global blocked registry */
// Add one proc to global list so matcher can see it
static void glob_add(Process *p) { glob_blocked[glob_blocked_count++] = p; }
// Remove one proc from global list
static void glob_remove(Process *p) {
    for (int i = 0; i < glob_blocked_count; ++i) {
        if (glob_blocked[i] == p) {
            for (int j = i; j < glob_blocked_count - 1; ++j)
                glob_blocked[j] = glob_blocked[j + 1];
            glob_blocked_count--;
            return;
        }
    }
}

/* --------- matching logic (cross-node) --------- */
// Try to match a sender with its receiver now
// On success both get scheduled for next tick on own nodes
static int try_match_now(Node *trigger_node, Process *p) {
    if (p->state != BLOCKED) return 0;

    int my_addr = proc_addr(p);

    if (p->want_dst_addr > 0) {
        // p is sender, seek waiting receiver q that expects p and sits at want_dst_addr
        int destN = addr_node(p->want_dst_addr);
        int destP = addr_pid (p->want_dst_addr);
        /* sender p: find a receiver q such that:
   - q is BLOCKED on RECV
   - p -> want_dst_addr == address(q)
   - q -> want_src_addr == address(p)
*/
        for (int i = 0; i < glob_blocked_count; ++i) {
            Process *q = glob_blocked[i];
            if (q == p || q->state != BLOCKED) continue;
            if (q->want_src_addr <= 0) continue;                 // q must be a receiver
            if (p->want_dst_addr != proc_addr(q)) continue;      // p targets q
            if (q->want_src_addr != proc_addr(p)) continue;      // q expects p

            // consume ops and update stats
            p->pc++; p->sends++;
            q->pc++; q->recvs++;

            Node *nd_s = &nodes[p->node];
            Node *nd_r = &nodes[q->node];

            remove_blocked(nd_s, p);
            remove_blocked(nd_r, q);
            glob_remove(p);
            glob_remove(q);

            int due = trigger_node->clock + 1;                   // release on next tick
            add_pending(nd_s, p, due, next_is_halt(p) ? 1 : 0);
            add_pending(nd_r, q, due, next_is_halt(q) ? 1 : 0);
            return 1;
        }

    } else if (p->want_src_addr > 0) {
        // p is receiver, seek sender s that names p and aims for p
        int srcN = addr_node(p->want_src_addr);
        int srcP = addr_pid (p->want_src_addr);
        /* receiver p: find a sender s such that:
   - s is BLOCKED on SEND
   - s -> want_dst_addr == address(p)
   - p -> want_src_addr == address(s)
*/
        for (int i = 0; i < glob_blocked_count; ++i) {
            Process *s = glob_blocked[i];
            if (s == p || s->state != BLOCKED) continue;
            if (s->want_dst_addr <= 0) continue;                 // s must be a sender
            if (s->want_dst_addr != proc_addr(p)) continue;      // s targets p
            if (p->want_src_addr != proc_addr(s)) continue;      // p expects s

            s->pc++; s->sends++;
            p->pc++; p->recvs++;

            Node *nd_s = &nodes[s->node];
            Node *nd_r = &nodes[p->node];

            remove_blocked(nd_s, s);
            remove_blocked(nd_r, p);
            glob_remove(s);
            glob_remove(p);

            int due = trigger_node->clock + 1;
            add_pending(nd_s, s, due, next_is_halt(s) ? 1 : 0);
            add_pending(nd_r, p, due, next_is_halt(p) ? 1 : 0);
            return 1;
        }

    }
    return 0;
}

// Search whole global list to create a match if possible
static int sweep_global_matches(void) {
    for (int i = 0; i < glob_blocked_count; ++i) {
        Process *a = glob_blocked[i];
        if (a->state != BLOCKED) continue;
        Node *nd = &nodes[a->node];
        if (try_match_now(nd, a)) return 1;
    }
    return 0;
}

/* --------- per-node time helpers --------- */
// Release any pending item due at current node clock
static int node_flush_pending(Node *nd) {
    int progress = 0;
    for (int i = 0; i < nd->pend_count; ) {
        Pending *e = &nd->pend[i];
        if (e->due_time == nd->clock) {
            Process *p = e->p;
            if (e->is_finish) {
                p->state = FINISHED;
                p->finish_time = nd->clock;
                print_state(nd->node_id, nd->clock, p->node_pid, "finished");
            } else {
                add_ready(nd, p);
            }
            // remove entry
            for (int j = i; j < nd->pend_count - 1; ++j) nd->pend[j] = nd->pend[j + 1];
            nd->pend_count--;
            progress = 1;
        } else {
            ++i;
        }
    }
    return progress;
}

// Wake procs that were BLOCKed with a time delay
static int node_expire_block(Node *nd) {
    int progress = 0;
    for (int i = 0; i < nd->blocked_count; ) {
        Process *p = nd->blocked[i];
        if (p->unblock_time > 0 && nd->clock >= p->unblock_time) {
            // timed BLOCK complete
            for (int j = i; j < nd->blocked_count - 1; ++j) nd->blocked[j] = nd->blocked[j + 1];
            nd->blocked_count--;
            // normal BLOCK is not in global list
            if (next_is_halt(p)) {
                p->pc++; // HALT costs zero ticks in this trace
                p->state = FINISHED;
                p->finish_time = nd->clock;
                print_state(nd->node_id, nd->clock, p->node_pid, "finished");
            } else {
                add_ready(nd, p);
            }
            progress = 1;
        } else {
            ++i;
        }
    }
    return progress;
}

/* run a single time slice on node nd using FIFO round robin */
// Handles DOOP work, then control ops that yield early
static int node_run_timeslice(Node *nd) {
    if (nd->ready_count == 0) return 0;

    Process *p = nd->ready[0];
    for (int j = 0; j < nd->ready_count - 1; ++j) nd->ready[j] = nd->ready[j + 1];
    nd->ready_count--;

    if (p->state == FINISHED || p->pc >= p->op_count) return 1;

    p->state = RUNNING;
    print_state(nd->node_id, nd->clock, p->node_pid, "running");

    int used = 0;
    int yielded = 0;

    while (used < nd->quantum && p->pc < p->op_count) {
        Operation *op = &p->ops[p->pc];

        if (op->type == DOOP) {
            int run_ticks = op->a;
            if (run_ticks > nd->quantum - used) run_ticks = nd->quantum - used;
            add_wait_ready(nd, run_ticks);
            p->run_time += run_ticks;
            nd->clock   += run_ticks;
            used        += run_ticks;
            op->a       -= run_ticks;
            if (op->a == 0) p->pc++;
        }
        else if (op->type == BLOCK) {
            int ticks = op->a;
            p->block_time   += ticks;
            p->unblock_time  = nd->clock + ticks;
            p->state         = BLOCKED;
            print_state(nd->node_id, nd->clock, p->node_pid, "blocked");
            p->pc++; // consume BLOCK
            add_blocked(nd, p);
            yielded = 1;
            break;
        }
        else if (op->type == SEND) {
            // one tick to attempt send, then block as sender
            add_wait_ready(nd, 1);
            p->run_time += 1;
            nd->clock += 1;
            used      += 1;

            p->want_dst_addr = op->a;
            p->want_src_addr = 0;
            p->unblock_time  = 0;
            p->state         = BLOCKED;
            print_state(nd->node_id, nd->clock, p->node_pid, "blocked (send)");
            add_blocked(nd, p);
            glob_add(p);
            (void)try_match_now(nd, p);
            yielded = 1;
            break;
        }
        else if (op->type == RECV) {
            // one tick to attempt recv, then block as receiver
            add_wait_ready(nd, 1);
            p->run_time += 1;          // account for this tick
            nd->clock += 1;
            used      += 1;

            p->want_src_addr = op->a;
            p->want_dst_addr = 0;
            p->unblock_time  = 0;
            p->state         = BLOCKED;
            print_state(nd->node_id, nd->clock, p->node_pid, "blocked (recv)");
            add_blocked(nd, p);
            glob_add(p);
            (void)try_match_now(nd, p);
            yielded = 1;
            break;
        }

        else if (op->type == HALT) {
            // HALT finishes at current time with zero cost
            p->pc++;
            p->state = FINISHED;
            p->finish_time = nd->clock;
            print_state(nd->node_id, nd->clock, p->node_pid, "finished");
            yielded = 1;
            break;
        }
        else {
            p->pc++; // safety advance on unknown op
        }
    }

    if (!yielded && p->state != FINISHED && p->pc < p->op_count) {
        p->wait_time += nd->quantum;
        add_ready(nd, p);
    }
    return 1;
}

/* advance node time to next event when no work is ready */
// Looks at pending due times and timed unblocks
static int node_advance_to_next_event(Node *nd) {
    int next_t = 0x3fffffff;
    int has = 0;

    for (int i = 0; i < nd->pend_count; ++i) {
        if (nd->pend[i].due_time > nd->clock && nd->pend[i].due_time < next_t) {
            next_t = nd->pend[i].due_time; has = 1;
        }
    }
    for (int i = 0; i < nd->blocked_count; ++i) {
        Process *p = nd->blocked[i];
        if (p->unblock_time > nd->clock && p->unblock_time < next_t) {
            next_t = p->unblock_time; has = 1;
        }
    }
    if (has) { nd->clock = next_t; return 1; }
    return 0;
}

// Stop when every node has no ready item, no blocked item, no pending entry
static int any_work_left(void) {
    for (int n = 1; n <= num_nodes; ++n) {
        Node *nd = &nodes[n];
        if (nd->ready_count > 0)   return 1;
        if (nd->blocked_count > 0) return 1;
        if (nd->pend_count > 0)    return 1;
    }
    return 0;
}

/* --------- main --------- */
int main(void) {
    // Input header: count of procs, count of nodes, quantum
    if (scanf("%d %d %d", &total_procs, &num_nodes, &quantum) != 3) return 0;


    for (int i = 0; i < total_procs; ++i) {
        // Read one process line then parse its program
        char name[32]; int size, prio, node_id;
        if (scanf("%31s %d %d %d", name, &size, &prio, &node_id) != 4) return 0;

        Process *p = &all_procs[i];
        strcpy(p->name, name);
        p->size = size; p->priority = prio; p->node = node_id;
        p->pid_global = i + 1;
        p->op_count = 0; p->pc = 0;
        p->state = NEW;
        p->run_time = p->block_time = p->wait_time = p->finish_time = 0;
        p->unblock_time = 0;
        p->sends = p->recvs = 0;
        p->want_dst_addr = 0; p->want_src_addr = 0;

        p->op_count = 0;
        p->pc = 0;
        /* Expand LOOP and END then stop at HALT */
        parse_block_into(p->ops, &p->op_count, 0);

    }

    // Init nodes then place procs into node bins
    for (int n = 1; n <= num_nodes; ++n) {
        nodes[n].node_id = n;
        nodes[n].quantum = quantum;
        nodes[n].clock = 0;
        nodes[n].proc_count = 0;
        nodes[n].ready_count = nodes[n].blocked_count = nodes[n].pend_count = 0;
    }
    int node_counts[MAX_NODES + 1] = {0};
    for (int i = 0; i < total_procs; ++i) {
        int n = all_procs[i].node;
        nodes[n].procs[nodes[n].proc_count++] = &all_procs[i];
        all_procs[i].node_pid = ++node_counts[n];
    }

    // Time zero log of NEW then mark all as READY
    for (int n = 1; n <= num_nodes; ++n) {
        Node *nd = &nodes[n];
        for (int i = 0; i < nd->proc_count; ++i) {
            Process *p = nd->procs[i];
            p->state = NEW;
            print_state(n, nd->clock, p->node_pid, "new");
        }
    }
    for (int n = 1; n <= num_nodes; ++n) {
        Node *nd = &nodes[n];
        for (int i = 0; i < nd->proc_count; ++i) add_ready(nd, nd->procs[i]);
    }

    // Main loop for all nodes using single logical time
    while (any_work_left()) {
        int progress = 0;

        // step one flush pending items that are due now
        for (int n = 1; n <= num_nodes; ++n) progress |= node_flush_pending(&nodes[n]);
        // step two expire timed BLOCKs if ready now
        for (int n = 1; n <= num_nodes; ++n) progress |= node_expire_block(&nodes[n]);
        // step three run one time slice per node in id order
        for (int n = 1; n <= num_nodes; ++n) progress |= node_run_timeslice(&nodes[n]);
        // step four try to create a SEND or RECV match if all nodes yielded
        if (!progress) progress |= sweep_global_matches();

        // step five if still stuck jump one node to next event
        if (!progress) {
            int best_node = -1, best_time = 0x3fffffff;
            for (int n = 1; n <= num_nodes; ++n) {
                Node *nd = &nodes[n];
                int t = 0x3fffffff; int has = 0;
                for (int i = 0; i < nd->pend_count; ++i)
                    if (nd->pend[i].due_time > nd->clock && nd->pend[i].due_time < t) { t = nd->pend[i].due_time; has = 1; }
                for (int i = 0; i < nd->blocked_count; ++i) {
                    Process *p = nd->blocked[i];
                    if (p->unblock_time > nd->clock && p->unblock_time < t) { t = p->unblock_time; has = 1; }
                }
                if (has && t < best_time) { best_time = t; best_node = n; }
            }
            if (best_node != -1) {
                nodes[best_node].clock = best_time;
                // do not flush now, next loop pass will handle it
                progress = 1;
            } else {
                break;
            }

        }

    }

    // Build summary rows then print sorted by finish time and tie breaks
    typedef struct { Process *p; int finish, node_id, node_pid, key; } Row;
    Row rows[MAX_PROCS * MAX_NODES]; int rc = 0;
    for (int n = 1; n <= num_nodes; ++n) {
        Node *nd = &nodes[n];
        for (int i = 0; i < nd->proc_count; ++i) {
            Process *p = nd->procs[i];
            if (p->state == FINISHED) {
                rows[rc].p = p;
                rows[rc].finish = p->finish_time;
                rows[rc].node_id = n;
                rows[rc].node_pid = p->node_pid;
                rows[rc].key = p->finish_time * 10000 + n * 100 + p->node_pid;
                rc++;
            }
        }
    }
    // Insertion sort is fine for small count
    for (int i = 1; i < rc; ++i) {
        Row x = rows[i]; int j = i - 1;
        while (j >= 0 && rows[j].key > x.key) { rows[j + 1] = rows[j]; --j; }
        rows[j + 1] = x;
    }
    for (int i = 0; i < rc; ++i) {
        Process *p = rows[i].p;
        printf("| %05d | Proc %02d.%02d | Run %d, Block %d, Wait %d, Sends %d, Recvs %d\n",
               rows[i].finish, rows[i].node_id, rows[i].node_pid,
               p->run_time, p->block_time, p->wait_time, p->sends, p->recvs);
    }
    return 0;
}
