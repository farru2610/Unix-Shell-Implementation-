#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#define MAX_INPUT_SIZE 1024
#define MAX_TOKEN_SIZE 64
#define MAX_NUM_TOKENS 64
#define MAX_BG_PROCS 64
#define MAX_SEQ_CMDS 64

/* ---------------- Tokenizer from starter (unaltered) ---------------- */
char **tokenize(char *line)
{
  char *tokens = (char *)malloc(MAX_NUM_TOKENS * sizeof(char *));
  char *token = (char *)malloc(MAX_TOKEN_SIZE * sizeof(char));
  int i, tokenIndex = 0, tokenNo = 0;

  for(i =0; i < (int)strlen(line); i++){
    char readChar = line[i];
    if (readChar == ' ' || readChar == '\n' || readChar == '\t'){
      token[tokenIndex] = '\0';
      if (tokenIndex != 0){
        tokens[tokenNo] = (char*)malloc(MAX_TOKEN_SIZE*sizeof(char));
        strcpy(tokens[tokenNo++], token);
        tokenIndex = 0;
      }
    } else {
      if (tokenIndex < MAX_TOKEN_SIZE-1)
        token[tokenIndex++] = readChar;
    }
  }

  free(token);
  tokens[tokenNo] = NULL ;
  return tokens;
}

/* ---------------- Globals for job control ---------------- */
static pid_t bg_pgid_list[MAX_BG_PROCS];
static int   bg_count = 0;

static pid_t fg_pgid = -1;   // active foreground process group id (serial or parallel)
static int   in_parallel = 0; // flag used to decide Ctrl+C behavior for E

/* Add/remove background pgid */
static void add_bg_pgid(pid_t pgid) {
  if (bg_count < MAX_BG_PROCS) bg_pgid_list[bg_count++] = pgid;
}

static void remove_bg_pgid(pid_t pgid) {
  for (int i = 0; i < bg_count; i++) {
    if (bg_pgid_list[i] == pgid) {
      bg_pgid_list[i] = bg_pgid_list[--bg_count];
      return;
    }
  }
}

/* ---------------- Signal handling ---------------- */
static void sigint_handler(int signo) {
  (void)signo;
  // Forward SIGINT only to current foreground group (if any).
  if (fg_pgid > 0) {
    kill(-fg_pgid, SIGINT); // negative => to process group
  }
  // Shell continues running.
}

/* Reap any finished background children without blocking */
static void reap_background(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // If the reaped pid belongs to a bg group, we may not know which;
    // still, notify and show exit status. We also try to cleanup pgid if no children remain.
    printf("Shell: Background process %d finished\n", pid);
    if (WIFEXITED(status)) {
      printf("EXITSTATUS: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      printf("TERMINATED BY SIGNAL: %d\n", WTERMSIG(status));
    }
    fflush(stdout);
  }
}

/* Prompt with CWD */
static void show_prompt(void) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    printf("%s $ ", cwd);
  } else {
    printf("$ ");
  }
  fflush(stdout);
}

/* Spawn a single process.
   - argv: NULL-terminated vector
   - background: 0/1
   - desired_pgid: if 0 => child uses its own pid as new pgid; else join given pgid
   Returns child's pid in parent; does not return in child. */
static pid_t spawn_process(char *argv[], int background, pid_t desired_pgid) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }
  if (pid == 0) {
    // Child: its own process group
    // For both fg and bg, we do setpgid so shell can target groups cleanly.
    pid_t child_pid = getpid();
    if (desired_pgid == 0) desired_pgid = child_pid;
    if (setpgid(0, desired_pgid) < 0) {
      perror("setpgid (child)");
      _exit(1);
    }

    // Foreground jobs should respond to Ctrl+C normally: restore default handler
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    execvp(argv[0], argv);
    perror("execvp");
    _exit(1);
  } else {
    // Parent: put child into the right pgid
    if (desired_pgid == 0) desired_pgid = pid;
    if (setpgid(pid, desired_pgid) < 0 && errno != EACCES && errno != ESRCH) {
      perror("setpgid (parent)");
    }

    if (background) {
      // track bg group so we can kill them on exit
      add_bg_pgid(desired_pgid);
    }
    return pid;
  }
}

/* Run one simple command: may be foreground or background, no && or &&& */
static void run_simple_command(char *tokens[], int background) {
  if (tokens[0] == NULL) return;

  // Builtins: cd / exit handled before this function is called.
  // Spawn as a new process group.
  pid_t pid = spawn_process(tokens, background, 0);
  if (pid < 0) return;

  if (!background) {
    // Foreground: wait for this exact child; still reap other finished bg in between
    int status;
    fg_pgid = pid;
    while (1) {
      pid_t w = waitpid(pid, &status, 0);
      if (w == -1 && errno == EINTR) {
        // Interrupted by signal; continue waiting. Reap any bg that finished.
        reap_background();
        continue;
      }
      break;
    }
    if (WIFEXITED(status)) {
      printf("EXITSTATUS: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      printf("TERMINATED BY SIGNAL: %d\n", WTERMSIG(status));
    }
    fg_pgid = -1;
  } else {
    printf("[bg] started pid=%d\n", pid);
  }
}

/* Parse tokens to detect & at end */
static int ends_with_ampersand(char *tokens[], int ntok) {
  if (ntok == 0) return 0;
  return strcmp(tokens[ntok-1], "&") == 0;
}

/* Builtins: returns 1 if handled, 0 otherwise */
static int handle_builtin(char *tokens[], int ntok) {
  if (ntok == 0) return 1;

  if (strcmp(tokens[0], "cd") == 0) {
    if (ntok != 2) {
      fprintf(stderr, "cd: expected 1 argument\n");
    } else if (chdir(tokens[1]) < 0) {
      perror("cd");
    }
    return 1;
  }

  if (strcmp(tokens[0], "exit") == 0) {
    // Kill all background groups, free, and exit
    for (int i = 0; i < bg_count; i++) {
      if (bg_pgid_list[i] > 0) kill(-bg_pgid_list[i], SIGKILL);
    }
    // a final reap (non-blocking)
    reap_background();
    exit(0);
  }

  return 0;
}

/* Collect commands separated by a separator token ("&&" or "&&&").
   Fills cmds[N][...] where each is a null-terminated argv vector built from original tokens.
   Returns number of commands. */
static int split_by_separator(char *tokens[], const char *sep, char *cmds[MAX_SEQ_CMDS][MAX_NUM_TOKENS]) {
  int ncmds = 0, idx = 0;
  for (int i = 0; i < MAX_SEQ_CMDS; i++) cmds[i][0] = NULL;

  int t = 0;
  while (tokens[t] != NULL) {
    if (strcmp(tokens[t], sep) == 0) {
      cmds[ncmds][idx] = NULL;
      ncmds++;
      idx = 0;
      if (ncmds >= MAX_SEQ_CMDS) break;
    } else {
      if (idx < MAX_NUM_TOKENS-1) {
        cmds[ncmds][idx++] = tokens[t];
      }
    }
    t++;
  }
  cmds[ncmds][idx] = NULL;
  ncmds++;
  return ncmds;
}

/* Serial execution: run each in foreground one after another */
static void run_serial(char *cmds[MAX_SEQ_CMDS][MAX_NUM_TOKENS], int ncmds) {
  for (int i = 0; i < ncmds; i++) {
    if (cmds[i][0] == NULL) continue;
    // No background allowed inside serial group by spec.
    run_simple_command(cmds[i], 0);
  }
}

/* Parallel execution: start all at once in one foreground group; wait for all */
static void run_parallel(char *cmds[MAX_SEQ_CMDS][MAX_NUM_TOKENS], int ncmds) {
  pid_t pids[MAX_SEQ_CMDS];
  int npids = 0;
  pid_t group_pgid = 0;

  in_parallel = 1;

  // spawn all, same pgid for group Ctrl+C
  for (int i = 0; i < ncmds; i++) {
    if (cmds[i][0] == NULL) continue;
    pid_t desired = (group_pgid == 0 ? 0 : group_pgid);
    pid_t pid = spawn_process(cmds[i], 0, desired);
    if (pid > 0) {
      if (group_pgid == 0) group_pgid = pid;
      pids[npids++] = pid;
    }
  }

  fg_pgid = group_pgid;

  // wait all; still reap random bg completions
  for (int i = 0; i < npids; i++) {
    int status;
    while (1) {
      pid_t w = waitpid(pids[i], &status, 0);
      if (w == -1 && errno == EINTR) {
        reap_background();
        continue;
      }
      break;
    }
    if (WIFEXITED(status)) {
      printf("EXITSTATUS: %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      printf("TERMINATED BY SIGNAL: %d\n", WTERMSIG(status));
    }
  }

  fg_pgid = -1;
  in_parallel = 0;
}

/* ---------------- Main Loop ---------------- */
int main(void) {
  // Setup SIGINT handler
  struct sigaction sa;
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; // restart interrupted syscalls like fgets
  sigaction(SIGINT, &sa, NULL);

  char line[MAX_INPUT_SIZE];

  while (1) {
    reap_background();
    show_prompt();

    // Use fgets to handle empty lines safely.
    if (fgets(line, sizeof(line), stdin) == NULL) {
      // EOF (Ctrl+D)
      printf("\n");
      break;
    }

    // If the user just hits Enter, continue
    int only_ws = 1;
    for (size_t i = 0; i < strlen(line); i++) {
      if (line[i] != ' ' && line[i] != '\t' && line[i] != '\n') { only_ws = 0; break; }
    }
    if (only_ws) continue;

    // The provided tokenizer expects a trailing newline; ensure present
    size_t len = strlen(line);
    if (len == 0 || line[len-1] != '\n') {
      if (len < sizeof(line)-1) {
        line[len] = '\n';
        line[len+1] = '\0';
      }
    }

    char **tokens = tokenize(line);

    // Count tokens
    int ntok = 0;
    while (tokens[ntok] != NULL) ntok++;

    if (ntok == 0) { // empty (shouldn't happen due to check, but safe)
      free(tokens);
      continue;
    }

    // Detect parallel "&&&" or serial "&&"
    int has_parallel = 0, has_serial = 0;
    for (int i = 0; i < ntok; i++) {
      if (strcmp(tokens[i], "&&&") == 0) has_parallel = 1;
      else if (strcmp(tokens[i], "&&") == 0) has_serial = 1;
    }

    if (has_parallel && has_serial) {
      fprintf(stderr, "Error: cannot mix && and &&& in one command line.\n");
      // free tokens
      for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
      free(tokens);
      continue;
    }

    if (has_parallel) {
      // Split by &&&
      char *cmds[MAX_SEQ_CMDS][MAX_NUM_TOKENS];
      int ncmds = split_by_separator(tokens, "&&&", cmds);

      // Disallow trailing/embedded '&' in parallel group by spec
      run_parallel(cmds, ncmds);

      for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
      free(tokens);
      continue;
    }

    if (has_serial) {
      // Split by &&
      char *cmds[MAX_SEQ_CMDS][MAX_NUM_TOKENS];
      int ncmds = split_by_separator(tokens, "&&", cmds);

      run_serial(cmds, ncmds);

      for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
      free(tokens);
      continue;
    }

    // Otherwise: single simple command (maybe background via '&')
    int background = 0;
    if (ends_with_ampersand(tokens, ntok)) {
      background = 1;
      free(tokens[ntok-1]);     // free "&"
      tokens[ntok-1] = NULL;    // remove it from argv
      ntok--;
    }

    // Builtins (only in foreground context)
    if (!background && handle_builtin(tokens, ntok)) {
      for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
      free(tokens);
      continue;
    }

    // If user typed "cd ... &" or "exit &" treat as error (keep simple)
    if (background && (strcmp(tokens[0], "cd") == 0 || strcmp(tokens[0], "exit") == 0)) {
      fprintf(stderr, "Error: builtin commands cannot run in background.\n");
      for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
      free(tokens);
      continue;
    }

    run_simple_command(tokens, background);

    // Cleanup token copies
    for (int i = 0; tokens[i] != NULL; i++) free(tokens[i]);
    free(tokens);
  }

  return 0;
}
