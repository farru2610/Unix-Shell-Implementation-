# Unix Shell Implementation  

A custom Unix-like shell implemented in **C** using **POSIX system calls**.  
This shell supports job control, signal handling, background/foreground execution, and execution of multiple commands in both **serial (`&&`)** and **parallel (`&&&`)** modes.  

---

## ✨ Features  
- **Custom Prompt** showing the current working directory  
- **Foreground & Background Execution** using `&`  
- **Serial Execution** (`cmd1 && cmd2`)  
- **Parallel Execution** (`cmd1 &&& cmd2 &&& cmd3`)  
- **Job Control** with process groups  
- **Signal Handling**  
  - `Ctrl+C` (SIGINT) only terminates active foreground jobs, not the shell  
- **Zombie Reaping** for background processes  
- **Exit Status Reporting** after command completion  
- **Built-in Commands**  
  - `cd <dir>` → change directory  
  - `exit` → terminate shell (kills background jobs before exit)  

---

## 🛠️ Build & Run  

### Compile  
```bash
gcc -Wall -Wextra -o mysh mysh.c
```

### Run  
```bash
./mysh
```

---

## 💡 Usage Examples  

- Run command in foreground:  
```bash
ls -l
```

- Run command in background:  
```bash
sleep 10 &
```

- Serial execution:  
```bash
echo first && echo second && echo third
```

- Parallel execution:  
```bash
echo A &&& echo B &&& echo C
```

- Builtins:  
```bash
cd ..
exit
```

---

## 📂 Code Highlights  
- **Process Management:** Uses `fork()`, `execvp()`, `waitpid()` for spawning and synchronizing processes  
- **Signal Handling:** Custom handlers for `SIGINT` ensuring stable foreground execution  
- **Job Control:** Background jobs are tracked and reaped to prevent zombies  
- **Error Handling:** Graceful handling of invalid commands and directories  
