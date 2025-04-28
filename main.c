#define _POSIX_C_SOURCE 2
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define FD_DIR "/proc/self/fd/"
#define DEV_NULL "/dev/null"
#define PID_FILE "/run/demon.pid"

// Main function of the daemon.
int daemon_main(int argc, char **argv)
{
    while (1) {
    }
    return 0;
}

int close_fds_dir(int skip_pipes[2])
{
    DIR *fddir;
    int error;
    int fd;
    int rc = 0;
    struct dirent *dentry;

    fddir = opendir(FD_DIR);
    if (fddir == NULL) {
        fprintf(stderr, "cannot open %s", FD_DIR);
        return 1;
    }

    while ((dentry = readdir(fddir)) != NULL) {
        if (dentry->d_name[0] == '.')
            continue;
        if (!strcmp(dentry->d_name, "0") || !strcmp(dentry->d_name, "1") || !strcmp(dentry->d_name, "2"))
            continue;

        fd = atoi(dentry->d_name);
        if (fd == 0) {
            fprintf(stderr, "could not parse file descriptor %s", dentry->d_name);
            rc = 1;
            goto exit;
        }
        if (fd == skip_pipes[0] || fd == skip_pipes[1])
            continue;

        error = close(fd);
        if (error) {
            fprintf(stderr, "could not close file descriptor %d", fd);
            rc = 1;
            goto exit;
        }
    }
exit:
    closedir(fddir);
    return rc;
}

int close_fds_limit(int skip_pipes[2])
{
    int error;
    struct rlimit rlimit;

    error = getrlimit(RLIMIT_NOFILE, &rlimit);
    if (error) {
        fprintf(stderr, "could not get rlimit");
        return 1;
    }

    for (int fd = 3; fd < rlimit.rlim_max; fd++) {
        if (fd == skip_pipes[0] || fd == skip_pipes[1])
            continue;
        error = close(fd);
        if (error) {
            switch (errno) {
            case EBADF:
                continue;
            default:
                perror("close");
                return 1;
            }
        }
    }
    return 0;
}

int reset_signals()
{
    void (*error)(int);
    for (int sig = 0; sig < _NSIG; sig++) {
        error = signal(sig, SIG_DFL);
        if (error == SIG_ERR)
            continue;
    }
    return 0;
}

int reset_signal_mask()
{
    sigset_t sigset;
    sigemptyset(&sigset);
    int error;

    error = sigprocmask(SIG_SETMASK, &sigset, NULL);
    if (error)
        perror("sigprocmask");
    return error;
}

int clean_environmnet()
{
    int error;
    const char *const unset_vars[] = {
        "USER",
        "LOGUSER",
        "HOME",
        "PATH",
        "TERM",
    };

    for (int i = 0; i < 5; i++) {
        error = unsetenv(unset_vars[i]);
        if (error)
            fprintf(stderr, "could not unset %s: %s", unset_vars[i], strerror(errno));
    }

    return 0;
}

void rmpidfile(void)
{
    unlink(PID_FILE);
}

void sighandler(int x)
{
    rmpidfile();
    exit(1);
}

int main(int argc, char *argv[])
{
    int error;
    pid_t child;
    int pipes[2] = { 0 };

    error = pipe(pipes);
    if (error) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    error = access(PID_FILE, F_OK);
    if (!error) {
        fprintf(stderr, "pid file exists, is the daemon already running?\n");
        return 1;
    }

    // Closing all file descriptors
    error = close_fds_dir(pipes);
    if (error)
        error = close_fds_limit(pipes);
    if (error)
        return EXIT_FAILURE;

    // Resetting signal handlers
    error = reset_signals();
    if (error)
        return EXIT_FAILURE;

    // Resetting process signal mask
    error = reset_signal_mask();
    if (error)
        return EXIT_FAILURE;

    // Sanitize environmental block
    error = clean_environmnet();
    if (error)
        return EXIT_FAILURE;

    // Create background process
    child = fork();
    if (child == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }

    // Code for the child process
    if (!child) {
        // New session.
        int child_error;
        child_error = setsid();
        if (child_error == -1) {
            perror("creating a new session");
            exit(1);
        }
        close(pipes[0]);

        // Double fork.
        pid_t child_pid;
        child_pid = fork();
        if (child_pid == -1) {
            perror("making second child");
            exit(1);
        }
        // Code for the daemon process
        if (!child_pid) {
            int null_fd;
            int e = -1;

            // Redirecting everything to /dev/null
            null_fd = open(DEV_NULL, O_RDWR);
            if (null_fd == -1) {
                perror("opening /dev/null");
                close(pipes[1]);
                close(null_fd);
                exit(1);
            }
            error = dup2(null_fd, 0);
            if (error == -1) {
                e = -1;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                close(null_fd);
                exit(1);
            }
            error = dup2(null_fd, 1);
            if (error == -1) {
                e = -2;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                close(null_fd);
                exit(1);
            }
            error = dup2(null_fd, 2);
            if (error == -1) {
                e = -3;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                close(null_fd);
                exit(1);
            }

            close(null_fd);

            error = chdir("/");
            if (error) {
                e = -4;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                exit(1);
            }

            // Here we write child `PID` to a file so that the daemon cannot be
            // launched more than once
            int pidfd;
            pid_t demon_pid = getpid();

            pidfd = open(PID_FILE, O_CREAT | O_EXCL | O_WRONLY);
            if (pidfd == -1) {
                e = -5;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                exit(1);
            }

            dprintf(pidfd, "%d", demon_pid);

            // File cleanup at exit.
            error = atexit(rmpidfile);
            if (error) {
                e = -6;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                close(pidfd);
                exit(1);
            }

            // File cleanup on termination.
            void (*sigerr)(int);
            sigerr = signal(SIGTERM, sighandler);
            if (sigerr == SIG_ERR) {
                e = -7;
                write(pipes[1], &e, sizeof(e));
                close(pipes[1]);
                close(pidfd);
                exit(1);
            }

            // Set `umask` to be 0
            umask(0);

            // Drop privileges (skipped);

            // Notify top parent of successful initialization
            write(pipes[1], &demon_pid, sizeof(demon_pid));
            close(pipes[1]);

            error = daemon_main(argc, argv);
            exit(error);
        } else {
            // Exit first child, so that daemon process gets parent `PID` 1.
            exit(0);
        }
    } else {
        pid_t daemon_pid;
        close(pipes[1]);

        // Read `PID` or error of the daemon.
        ssize_t rbytes = read(pipes[0], &daemon_pid, sizeof(daemon_pid));
        if (rbytes != sizeof(daemon_pid)) {
            fprintf(stderr, "unable to confirm daemon is running\n");
            return 255;
        }
        close(pipes[0]);
        if (daemon_pid < 0) {
            fprintf(stdout, "dameon failed with code: %d\n", daemon_pid);
            return EXIT_FAILURE;
        }

        fprintf(stdout, "dameon running with pid: %d\n", daemon_pid);

        return EXIT_SUCCESS;
    }
}
