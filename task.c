#include<sys/socket.h>
#include<sys/stat.h>
#include<stdio.h>
#include<signal.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<fcntl.h>

// global flag for signal handler to indicate termination of program
volatile sig_atomic_t end = 0;


struct slot {
	unsigned int count; 
	char *string;
};


struct dict {
	unsigned long long size;
	struct slot *slots;

};


struct dict init_dict(unsigned long long dict_size) {
	// calloc, so default count value is zero
	struct dict dictionary = {dict_size,  calloc(sizeof(struct slot),  dict_size)};
	return dictionary;
};


void free_dict(struct dict *dict) {
	for (unsigned long long i = 0; i < dict->size; i++) {
		if (dict->slots[i].count != 0) {
			free(dict->slots[i].string);
		}
	}
}

// ngl, I stole this hash function from chatgpt.
unsigned long long hash(const char* str) {
	unsigned long long hash_val = 0;
	unsigned int i = 0;

	for (int i = 0; str[i] != '\0'; i++) {
		hash_val = hash_val * 31 + str[i];

	}

	return hash_val;
}

// Let's pretend hash collision doesn't occur
void push_string_dict(struct dict *dict, const char *str) {
	unsigned long long hash_val = hash(str);
	unsigned long long location = hash_val % dict->size;

	if (dict->slots[location].count == 0) {
		dict->slots[location].string = strdup(str);
		if (dict->slots[location].string == NULL) {
			perror("failed to strdup");
			exit(1);

		}
	}
	dict->slots[location].count += 1;
}

// in case of mutliple messages having the same number of occurences, show the first message,
// which is actually random message, user doesn't know the internal order of dictionary
// returns numbers of occurence, most_common_str points to string in dict, does not copy string
unsigned long long most_common_msg(struct dict *dict, char **most_common_str) {
	unsigned int max = 0;
	unsigned int max_index = 0;

	for (unsigned long long i = 0; i < dict->size; i++) {
		if (dict->slots[i].count > max) {
			max = dict->slots[i].count;
			max_index = i;
		}
	}

	*most_common_str = dict->slots[max_index].string;
	return max;
}


static void handler(int sig) {
	if (sig == SIGINT) {
		end = 1;
	}
}

// Heavily inspired from The Linux Programming interface
void become_daemon() {
	switch (fork()) {
		// error
		case -1: 
			perror("failed to create child");
			exit(1);
		// child
		case 0:
			break;
		// Kill parent
		default:
			exit(0);
	}


	if (setsid() == -1) {
		perror("failed to became leader of new session");
		exit(1);
	}

	// kill parent so we are not session leader
	switch (fork()) {
		case -1: 
			perror("failed to create child");
			exit(1);
		// child
		case 0:
			break;
		// Kill parent
		default:
			exit(0);
	}


	chdir("/");

	umask(0);

	// Close all open file descriptors
	int maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd == -1)
		maxfd = 8192;
	for (int fd = 0; fd < maxfd; fd++)
		close(fd);



	// Redirect STDIN, STDOUT, STDERR to /dev/null
	int fd = open("/dev/null", O_RDWR);
	if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO) {
		perror("failed to duplicate fd0 to fd1");
		exit(1);
	}
	if (dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO) {
		perror("failed to duplicate fd1 to fd2");
		exit(1);
	}

}

int main(int argc, char *argv[]) {
	// The hash map is jut a toy hash map
	// and 999 is enough for everyone
	struct dict dict = init_dict(999);
	if (dict.slots == NULL) {
		printf("Failed to allocate dictionary\n");
		exit(1);
	}

	int fork_mode = 0;
	if (argc > 2  && strcmp(argv[1], "-f") == 0) {
		fork_mode = 1;
		become_daemon();
	}


	// open logs files
	int files_to_open = argc - 1 - fork_mode;
	FILE *logs[files_to_open];
	for (int i = 1 + fork_mode; i < files_to_open + 1 +fork_mode; i++) {
		logs[i - 1 - fork_mode] = fopen(argv[i], "w");
		if (logs[i - 1 - fork_mode] == NULL) {
			perror("failed to open file");
			exit(1);
		}
	}

	if (access("/dev/log", F_OK) == 0) {
		if (unlink("/dev/log") != 0) {
			perror("unlinking of /dev/log failed, are you root?");
			exit(1);
		}
	}



	// Create /dev/log socket
	struct sockaddr addr = {AF_UNIX, "/dev/log"};

	int dev_log_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (bind(dev_log_fd, &addr, sizeof(addr)) != 0) {
		perror("Failed to link /dev/log, are you root?");
		exit(1);
	} else {
		// Allow write by others
		chmod("/dev/log", S_IWOTH);
	}


	// Block all signals except SIGINT
	sigset_t set;
	sigfillset(&set);
	sigdelset(&set, SIGINT);
	sigprocmask(SIG_BLOCK, &set, NULL);


	// set handler for SIGINT
	struct sigaction sa;
	sa.sa_handler = handler;
	sa.sa_mask = set;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);


	char buf[401];
	int num;
	while(end == 0 && read(dev_log_fd, buf, 400) > -1) {
		// Add null byte at the end, in case user try to send longer message than we accept
		// ommiting the null byte, protection against memory leak
		buf[400] = '\0';
		// Well we print max 400 chars anyway :D
		printf("%.400s\n", buf);

		char *stripped_msg = buf;
		// Example message
		// <13>May 14 01:42:39 filip: SPRAVA
		// strip three spaces
		for (int i = 0; i < 3; i += 1) {
			stripped_msg = strchr(stripped_msg, ' ') + 1;
			if (stripped_msg == NULL + 1) {
				printf("logging message is badly formated, exiting\n");
				exit(1);
			}
		}


		for (int i = 0; i < files_to_open; i++) {
			fprintf(logs[i],"%.397s\n", stripped_msg);
		}

		push_string_dict(&dict, stripped_msg);
	}

	char *most_common;
	unsigned long long occurences = most_common_msg(&dict, &most_common);
	// Well, max len of message is 400, minus 3 whitespaces, so print at max 397 chars, security by obscurity :D
	if (occurences == 0) {
		printf("No messages\n"); 
	} else {
		printf("%llu --> %.397s", occurences, most_common); 
	}
	free_dict(&dict);
	unlink("/dev/log");

	// Close all open log files
	for (int i = 0; i < files_to_open - 1; i++) {
		fclose(logs[i]);
	}
}
