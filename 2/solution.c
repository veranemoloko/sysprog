#include "../utils/heap_help/heap_help.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "parser.h"

typedef enum {
  SINGLE_CMD,
  FIRST_CMD,
  MIDDLE_CMD,
  LAST_CMD,
  START_CMD
} cmdStage;

int exec_cmd(const struct command cmd, cmdStage stage, int *to_parent,
             const struct command_line line) {
  int exit_stat = EXIT_SUCCESS;
  int to_child[2];
  pipe(to_child);
  int fd_out =
      line.out_type == OUTPUT_TYPE_STDOUT ? STDOUT_FILENO
      : line.out_type == OUTPUT_TYPE_FILE_NEW
          ? open(line.out_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)
          : open(line.out_file, O_WRONLY | O_CREAT | O_APPEND,
                 S_IRUSR | S_IWUSR);
  // printf(" fd_out = %d\n", fd_out);

  if (fork() == 0) {

    if (stage != SINGLE_CMD) {
      if (stage != FIRST_CMD) {
        dup2(to_parent[0], STDIN_FILENO);
      }
      if (stage != LAST_CMD) {
        dup2(to_child[1], STDOUT_FILENO);
      }
    }
    if (stage == SINGLE_CMD || stage == LAST_CMD) {
      dup2(fd_out, STDOUT_FILENO);
    }
    close(to_child[0]);
    close(to_child[1]);
    close(to_parent[0]);
    close(to_parent[1]);

    char *exec_args[cmd.arg_count + 2];
    exec_args[0] = cmd.exe;
    memcpy(exec_args + 1, cmd.args, sizeof(exec_args[0]) * cmd.arg_count);
    exec_args[cmd.arg_count + 1] = NULL;
    exit_stat = execvp(cmd.exe, exec_args);

    exit(exit_stat);
  }
  if (stage != FIRST_CMD) {
    close(to_parent[0]);
    close(to_parent[1]);
  }
  if (stage == SINGLE_CMD || stage == LAST_CMD) {
    close(to_child[0]);
    close(to_child[1]);
  } else {
    to_parent[0] = to_child[0];
    to_parent[1] = to_child[1];
  }
  return exit_stat;
}

int change_stage(const struct expr *e, int last_stage) {
  if (e->next == NULL) {
    return (last_stage == MIDDLE_CMD || last_stage == FIRST_CMD) ? LAST_CMD
                                                                 : SINGLE_CMD;
  } else if (e->next->type == EXPR_TYPE_PIPE) {

    return (last_stage == FIRST_CMD || last_stage == MIDDLE_CMD) ? MIDDLE_CMD
                                                                 : FIRST_CMD;
  } else {

    return (last_stage == FIRST_CMD || last_stage == MIDDLE_CMD) ? LAST_CMD
                                                                 : SINGLE_CMD;
  }
}

int execute_command_line(const struct command_line *line, bool *is_exit) {
  int exit_stat = EXIT_SUCCESS;
  assert(line != NULL);
  const struct expr *e = line->head;

  int to_parent[2];
  pipe(to_parent);

  cmdStage stage = START_CMD;
  while (e != NULL) {

    if (e->type == EXPR_TYPE_PIPE)
      e = e->next;
    else if (e->type == EXPR_TYPE_AND) {
      if (exit_stat != EXIT_SUCCESS)
        break;
      e = e->next;
    } else if (e->type == EXPR_TYPE_OR) {
      if (exit_stat == EXIT_SUCCESS)
        break;
      e = e->next;
    }

    stage = change_stage(e, stage);

    if (strcmp(e->cmd.exe, "exit") == 0) {
      if (stage == SINGLE_CMD) {
        *is_exit = true;
        while (wait(NULL) > 0)
          ;
        return exit_stat;
      }
    }

    // printf("\n--- e=%s stage=%d", e->cmd.exe, stage);
    if (strcmp(e->cmd.exe, "cd") == 0) {
      if (stage == SINGLE_CMD) {
        if (e->cmd.arg_count > 0)
          exit_stat = chdir(e->cmd.args[0]);
        else
          exit_stat = 1;
        // printf(" exit = %d", exit_stat);
      }
    } else
      exit_stat = exec_cmd(e->cmd, stage, to_parent, *line);

    if (stage == LAST_CMD || stage == SINGLE_CMD) {

      int status;
      if (waitpid(-1, &status, 0) > 0) {
        if (WIFEXITED(status)) {
          exit_stat = WEXITSTATUS(status);
          //   printf(" status==%d", exit_stat);
        }
      }
    }

    e = e->next;
  }

  close(to_parent[0]);
  close(to_parent[1]);
  // printf("\n__________________________\n");
  return exit_stat;
}

int main() {
  bool is_exit = false;
  int exit_stat = EXIT_SUCCESS;
  const size_t buf_size = 1024;
  char buf[buf_size];
  int rc;
  struct parser *p = parser_new();
  while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
    parser_feed(p, buf, rc);
    struct command_line *line = NULL;
    while (true) {
      enum parser_error err = parser_pop_next(p, &line);
      if (err == PARSER_ERR_NONE && line == NULL)
        break;
      if (err != PARSER_ERR_NONE) {
        // printf("Error: %d\n", (int)err);
        continue;
      }
      exit_stat = execute_command_line(line, &is_exit);
      command_line_delete(line);
    }
    if (is_exit)
      break;
  }
  parser_delete(p);
  exit(exit_stat);
}