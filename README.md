# 🧩 ChronoSim — Multithreaded Distributed Process Simulator in C

**ChronoSim** is a multithreaded, distributed process simulation framework written in C.  
It models the internal behavior of an operating system — from single-process execution to full multi-node synchronization — including CPU scheduling, message passing, and clock coordination across threads.

---

## 🧠 Purpose

ChronoSim demonstrates how key operating system components work together in simulated form:  
- **CPU scheduling** (Round Robin, Priority-based, Shortest Job First)  
- **Thread synchronization** using custom barrier monitors  
- **Inter-process communication (IPC)** via synchronous `SEND` and `RECV` primitives  
- **Clock alignment** across multiple simulated nodes  

It provides a modular and educational view of scheduling, concurrency, and synchronization concepts implemented entirely in portable C.

---

## 🚀 Features

### 🧩 Stage 1 — Program Execution Simulator
- Interprets primitives: `DOOP`, `BLOCK`, `LOOP`, `END`, and `HALT`.
- Tracks CPU ticks and outputs precise execution timelines.
- Implements stack-based nested loop management.

### 🧩 Stage 2 — Process Scheduler
- Simulates multiple concurrent processes on a single CPU.
- Implements Round Robin, SJF, and Priority Scheduling.
- Uses ready and blocked queues (linked list priority queues).
- Tracks run time, block time, and wait time per process.

### 🧩 Stage 3 — Multi-Node Simulation
- Spawns one thread per node using POSIX threads (`pthread`).
- Each node maintains its own ready/blocked queues and clock.
- Thread-safe output using mutex-protected shared queues.

### 🧩 Stage 4 — Synchronized Distributed Execution
- Adds custom barrier synchronization to align clocks across all nodes.
- Implements synchronous message passing via `SEND` and `RECV`.
- Uses encoded addresses (`NodeID * 100 + ProcessID`) for routing messages.
- Tracks send/receive counts per process.
- Produces fully synchronized per-tick simulation output.

---

## 🧩 Input Format

The simulator reads input from `stdin`. Example:

```
4 5 2
Proc1 3 1 1
SEND 201
RECV 202
HALT

Proc2 3 1 1
RECV 202
SEND 201
HALT

Proc3 3 1 2
RECV 101
RECV 102
HALT

Proc4 3 1 2
SEND 102
SEND 101
HALT
```

---

## 🖥️ Example Output

```
[01] 00000: process 1 new
[01] 00000: process 1 ready
[01] 00000: process 1 running
[01] 00001: process 1 blocked (send)
[02] 00000: process 1 new
[02] 00000: process 1 running
[02] 00001: process 1 blocked (recv)
| 00005 | Proc 01.01 | Run 2, Block 0, Wait 0, Sends 1, Recvs 1
| 00005 | Proc 02.01 | Run 2, Block 0, Wait 1, Sends 1, Recvs 1
```

---

## 🧰 Build & Run

### 🏗️ Build
```bash
git clone https://github.com/MaafiaTroodon/MultiProSim.git
cd MultiProSim
make
```

### ▶️ Run
```bash
./prosim < input.txt
```

To test different configurations, modify `input.txt` with desired process descriptions.

---

## 🧩 Implementation Details

- Each node is represented by a thread executing its local simulation loop.  
- Barriers synchronize node clocks before every tick increment.  
- `send()` and `recv()` functions block until both sender and receiver are ready.  
- Output is globally ordered by finish time, node ID, and process ID.  
- Statistics summarize execution metrics for all processes.

---

## 📊 Output Summary Format

```
| Time | Proc Node.PID | Run run_time, Block block_time, Wait wait_time, Sends send_count, Recvs recv_count
```

---

## 🧑‍💻 Author

**Malhar Datta Mahajan**  
📫 [LinkedIn](https://linkedin.com/in/malharmahajan) · [GitHub](https://github.com/MaafiaTroodon)

---

## 🪪 License

This project is open-source under the [MIT License](LICENSE).

---

### 💡 Inspiration
ChronoSim was developed as a complete progression from a simple CPU simulator to a fully concurrent multi-threaded distributed system — exploring how real operating systems synchronize processes, manage states, and exchange messages.
