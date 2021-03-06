//  nbudstee
//
//  WEBSITE: https://github.com/JGRennison/nbudstee
//  WEBSITE: https://bitbucket.org/JGRennison/nbudstee
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version. See: COPYING-GPL.txt
//
//  This program  is distributed in the  hope that it will  be useful, but
//  WITHOUT   ANY  WARRANTY;   without  even   the  implied   warranty  of
//  MERCHANTABILITY  or FITNESS  FOR A  PARTICULAR PURPOSE.   See  the GNU
//  General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//  2014 - Jonathan G Rennison <j.g.rennison@gmail.com>
//==========================================================================

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <vector>
#include <memory>
#include <deque>
#include <string>

#ifndef VERSION_STRING
#define VERSION_STRING __DATE__ " " __TIME__
#endif
const char version_string[] = "nbudstee " VERSION_STRING;
const char authors[] = "Written by Jonathan G. Rennison <j.g.rennison@gmail.com>";

enum class FDTYPE {
	NONE,
	INPUT,
	LISTENER,
	CONN,
	FIFO,
};

struct fd_out_buffer {
	std::shared_ptr<std::vector<unsigned char> > buffer;
	size_t offset;
};

struct fdinfo {
	FDTYPE type = FDTYPE::NONE;
	unsigned int pollfd_offset;
	std::string name;
	std::deque<fd_out_buffer> out_buffers;
	size_t buffered_data = 0;
	bool have_overflowed = false;
	void clear() {
		out_buffers.clear();
	}
};

bool force_exit = false;
bool use_stdout = true;
size_t max_queue = 65536;
bool remove_after = false;
bool remove_before = false;
bool no_overflow = false;
std::vector<struct pollfd> pollfds;
std::deque<struct fdinfo> fdinfos;

int input_fd = STDIN_FILENO;
const char *input_name = "STDIN";
bool reopen_input = false;

const size_t buffer_count_shrink_threshold = 4;

void addpollfd(int fd, short events, FDTYPE type, std::string name) {
	if(fdinfos.size() <= (size_t) fd) fdinfos.resize(fd + 1);
	if(fdinfos[fd].type != FDTYPE::NONE) {
		fprintf(stderr, "Attempt to add duplicate fd to poll array detected, ignoring: fd: %d\n", fd);
		return;
	}
	fdinfos[fd].type = type;
	fdinfos[fd].pollfd_offset = pollfds.size();
	fdinfos[fd].name = std::move(name);

	pollfds.push_back({ fd, events, 0 });
}

void delpollfd(int fd) {
	if((size_t) fd >= fdinfos.size() || fdinfos[fd].type == FDTYPE::NONE) {
		fprintf(stderr, "Attempt to remove non-existant fd from poll array detected, ignoring: fd: %d\n", fd);
		return;
	}

	size_t offset = fdinfos[fd].pollfd_offset;
	//offset is poll slot of fd currently being removed

	//if slot is not the last one, move the last one in to fill empty slot
	if(offset < pollfds.size() - 1) {
		pollfds[offset] = std::move(pollfds.back());
		int new_fd_in_slot = pollfds[offset].fd;
		fdinfos[new_fd_in_slot].pollfd_offset = offset;
	}
	pollfds.pop_back();
	fdinfos[fd].type = FDTYPE::NONE;
	fdinfos[fd].clear();
}

void setpollfdevents(int fd, short events) {
	size_t offset = fdinfos[fd].pollfd_offset;
	pollfds[offset].events = events;
}

void setnonblock(int fd, const char *name) {
	int flags = fcntl(fd, F_GETFL, 0);
	int res = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if(flags < 0 || res < 0) {
		fprintf(stderr, "Could not fcntl set O_NONBLOCK %s: %m\n", name);
		exit(1);
	}
}

std::deque<std::shared_ptr<std::vector<unsigned char> > > free_buffers;

std::shared_ptr<std::vector<unsigned char> > getbuffer() {
	if(free_buffers.empty()) return std::make_shared<std::vector<unsigned char> >();
	else {
		std::shared_ptr<std::vector<unsigned char> > buffer = std::move(free_buffers.back());
		free_buffers.pop_back();
		return std::move(buffer);
	}
}

void finished_with_buffer(std::shared_ptr<std::vector<unsigned char> > buffer) {
	if(buffer.unique()) {
		free_buffers.emplace_back(std::move(buffer));
	}
}

void cleanup() {
	if(remove_after) {
		for(auto &it : fdinfos) {
			if(it.type == FDTYPE::LISTENER) {
				unlink(it.name.c_str());
			}
			else if(it.type == FDTYPE::FIFO) {
				unlink(it.name.c_str());
			}
		}
	}
}

void open_named_input() {
	input_fd = open(input_name, O_NONBLOCK | O_RDONLY);
	if(input_fd == -1) {
		fprintf(stderr, "Failed to open '%s' for input, %m. Exiting.\n", input_name);
		cleanup();
		exit(1);
	}
}

std::shared_ptr<std::vector<unsigned char> > read_input_fd(int fd, bool &continue_flag) {
	std::shared_ptr<std::vector<unsigned char> > buffer = getbuffer();
	buffer->resize(4096);

	ssize_t bread = 0;
	while(!force_exit) {
		bread = read(fd, buffer->data(), buffer->size());
		if(bread < 0) {
			if(errno == EINTR) {
				bread = 0;
				continue;
			}
			fprintf(stderr, "Failed to read from STDIN: %m\n");
			cleanup();
			exit(1);
		}
		break;
	}

	if(bread == 0) {
		if(reopen_input && fd == input_fd) {
			close(fd);
			delpollfd(fd);
			open_named_input();
			setnonblock(input_fd, input_name);
			addpollfd(input_fd, POLLIN | POLLERR, FDTYPE::INPUT, input_name);
			continue_flag = false;
			return std::shared_ptr<std::vector<unsigned char> >();
		}
		else {
			cleanup();
			exit(0);
		}
	}
	else if(bread > 0) {
		std::vector<int> pending_close_fds;

		buffer->resize(bread);
		for(int fd = 0; fd < (int) fdinfos.size(); fd++) {
			if(fdinfos[fd].type == FDTYPE::CONN || fdinfos[fd].type == FDTYPE::FIFO) {
				if(fdinfos[fd].buffered_data < max_queue) {
					fdinfos[fd].out_buffers.push_back({ buffer, 0 });
					if(fdinfos[fd].out_buffers.size() >= buffer_count_shrink_threshold) {
						//Starting to accumulate a lot of buffers
						//Shrink to fit the older ones to avoid storing large numbers of potentially mostly empty buffers
						fdinfos[fd].out_buffers[fdinfos[fd].out_buffers.size() - buffer_count_shrink_threshold].buffer->shrink_to_fit();
					}
					fdinfos[fd].buffered_data += buffer->size();
					setpollfdevents(fd, POLLOUT | POLLERR);
				}
				else if(!fdinfos[fd].have_overflowed) {
					fdinfos[fd].have_overflowed = true;
					if(no_overflow) {
						// Don't close here as we are currently iterating over the list of fds
						pending_close_fds.push_back(fd);
						fprintf(stderr, "Queue overflow for output: %s, closing connection\n", fdinfos[fd].name.c_str());
					}
					else {
						fprintf(stderr, "Queue overflow for output: %s\n", fdinfos[fd].name.c_str());
					}
				}
			}
		}

		if(pending_close_fds.size()) {
			continue_flag = false;
			for(auto &fd : pending_close_fds) {
				close(fd);
				delpollfd(fd);
			}
		}
	}
	return std::move(buffer);
}

void sighandler(int sig) {
	force_exit = true;
}

static struct option options[] = {
	{ "help",          no_argument,        NULL, 'h' },
	{ "no-stdout",     no_argument,        NULL, 'n' },
	{ "unlink-after",  no_argument,        NULL, 'u' },
	{ "unlink-before", no_argument,        NULL, 'b' },
	{ "max-queue",     required_argument,  NULL, 'm' },
	{ "input",         required_argument,  NULL, 'i' },
	{ "input-reopen",  required_argument,  NULL, 'I' },
	{ "version",       no_argument,        NULL, 'V' },
	{ "no-overflow",   no_argument,        NULL, 'd' },
	{ NULL, 0, 0, 0 }
};

int main(int argc, char **argv) {
	int n = 0;
	while (n >= 0) {
		n = getopt_long(argc, argv, "hnubm:i:I:Vd", options, NULL);
		if (n < 0) continue;
		switch (n) {
		case 'n':
			use_stdout = false;
			break;
		case 'u':
			remove_after = true;
			break;
		case 'b':
			remove_before = true;
			break;
		case 'm': {
			char *end = 0;
			max_queue = strtoul(optarg, &end, 0);
			if(!end) { /* do nothing*/ }
			else if(!*end) { /* valid integer */ }
			else if(end == std::string("k")) max_queue <<= 10;
			else if(end == std::string("M")) max_queue <<= 20;
			else if(end == std::string("G")) max_queue <<= 30;
			else {
				fprintf(stderr, "Invalid max queue length: '%s'\n", optarg);
			}
			break;
		}
		case 'I':
			reopen_input = true;
			//fall through
		case 'i':
			input_name = optarg;
			open_named_input();
			break;
		case 'd':
			no_overflow = true;
			break;
		case 'V':
			fprintf(stdout, "%s\n\n%s\n", version_string, authors);
			exit(0);
		case '?':
		case 'h':
			fprintf(n == '?' ? stderr : stdout,
					"Usage: nbudstee [options] [uds ...]\n"
					"\tCopy Input to zero or more non-blocking Unix domain sockets\n"
					"\teach of which can have zero or more connected readers, and/or to zero or more\n"
					"\texisting FIFOs, each of which can have exactly one existing reader.\n"
					"\tInput defaults to STDIN.\n"
					"\tAlso copies to STDOUT, unless -n, --no-stdout is used.\n"
					"\tNo attempt is made to line-buffer or coalesce the input.\n"
					"Options:\n"
					"-n, --no-stdout\n"
					"\tDo not copy input to STDOUT.\n"
					"-b, --unlink-before\n"
					"\tFirst try to unlink any existing sockets. This will not try to unlink non-sockets.\n"
					"-u, --unlink-after\n"
					"\tTry to unlink all sockets and FIFOs when done.\n"
					"-m, --max-queue bytes\n"
					"\tMaximum amount of data to buffer for each connected socket reader (approximate).\n"
					"\tAccepts suffixes: k, M, G, for multiples of 1024. Default: 64k.\n"
					"\tAbove this limit new data for that socket reader will be discarded,\n"
					"\t(unless -d/--no-overflow is used).\n"
					"-d, --no-overflow\n"
					"\tDisconnect readers which would otherwise have data discarded because their\n"
					"\tbuffer is full.\n"
					"-i, --input file\n"
					"\tRead from file instead of STDIN.\n"
					"-I, --input-reopen file\n"
					"\tRead from file instead of STDIN.\n"
					"\tWhen the end of input is reached, reopen from the beginning.\n"
					"\tThis is primarily intended for FIFOs.\n"
					"-h, --help\n"
					"\tShow this help\n"
					"-V, --version\n"
					"\tShow version information\n"
			);
			exit(n == '?' ? 1 : 0);
		}
	}

	struct sigaction new_action;
	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = sighandler;
	sigaction(SIGINT, &new_action, 0);
	sigaction(SIGHUP, &new_action, 0);
	sigaction(SIGTERM, &new_action, 0);
	new_action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &new_action, 0);

	setnonblock(input_fd, input_name);
	addpollfd(input_fd, POLLIN | POLLERR, FDTYPE::INPUT, input_name);

	while (optind < argc) {
		const char *name = argv[optind++];

		struct stat sf;
		int stat_result = stat(name, &sf);
		if((stat_result != -1) && (S_ISFIFO(sf.st_mode))) {
			int fd = open(name, O_NONBLOCK | O_WRONLY | O_APPEND);
			if(fd == -1) {
				fprintf(stderr, "FIFO: %s cannot be opened, %m\n", name);
				continue;
			}
			addpollfd(fd, POLLERR, FDTYPE::FIFO, name);
		} else {
			int sock = socket(AF_UNIX, SOCK_STREAM, 0);
			if(sock == -1) {
				fprintf(stderr, "socket() failed, %m\n");
				continue;
			}
			struct sockaddr_un my_addr;
			memset(&my_addr, 0, sizeof(my_addr));
			my_addr.sun_family = AF_UNIX;
			size_t maxlen = sizeof(my_addr.sun_path) - 1;
			if(strlen(name) > maxlen) {
				fprintf(stderr, "Socket name: %s too long, maximum: %zu\n", name, maxlen);
				exit(1);
			}
			strncpy(my_addr.sun_path, name, maxlen);

			if(remove_before) {
				if(stat_result != -1) {
					if(S_ISSOCK(sf.st_mode)) {
						//only try to unlink if the existing file is a socket
						unlink(name);
					}
				}
			}

			if(bind(sock, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1) {
				fprintf(stderr, "bind(%s) failed, %m\n", name);
				continue;
			}

			if(listen(sock, 64) == -1) {
				fprintf(stderr, "listen(%s) failed, %m\n", name);
				continue;
			}

			setnonblock(sock, name);
			addpollfd(sock, POLLIN | POLLERR, FDTYPE::LISTENER, name);
		}
	}

	while(!force_exit) {
		int n = poll(pollfds.data(), pollfds.size(), -1);
		if(n < 0) {
			if(errno == EINTR) continue;
			else break;
		}

		bool continue_flag = true;

		for(size_t i = 0; i < pollfds.size() && continue_flag; i++) {
			if(!pollfds[i].revents) continue;
			int fd = pollfds[i].fd;
			switch(fdinfos[fd].type) {
				case FDTYPE::NONE:
					exit(2);
				case FDTYPE::INPUT: {
					auto buffer = read_input_fd(fd, continue_flag);
					if(!buffer) break;
					if(use_stdout) {
						size_t offset = 0;
						while(buffer->size() > offset && !force_exit) {
							ssize_t result = write(STDOUT_FILENO, buffer->data() + offset, buffer->size() - offset);
							if(result < 0) {
								if(errno == EINTR) continue;
								fprintf(stderr, "Write to STDOUT failed, %m. Exiting.\n");
								cleanup();
								exit(1);
							}
							offset += result;
						}
					}
					finished_with_buffer(std::move(buffer));
					break;
				}
				case FDTYPE::LISTENER: {
					int newsock = accept(fd, 0, 0);
					if(newsock == -1) {
						fprintf(stderr, "accept(%s) failed, %m\n", fdinfos[fd].name.c_str());
						cleanup();
						exit(1);
					}
					setnonblock(newsock, fdinfos[fd].name.c_str());
					addpollfd(newsock, POLLERR, FDTYPE::CONN, fdinfos[fd].name);
					break;
				}
				case FDTYPE::CONN:
				case FDTYPE::FIFO: {
					auto &out_buffers = fdinfos[fd].out_buffers;
					if(!(pollfds[i].revents & POLLOUT)) {
						close(fd);
						delpollfd(fd);
						continue_flag = false;
						break;
					}
					if(out_buffers.empty()) {
						pollfds[i].events = POLLERR;
						continue;
					}
					auto buffer_item = std::move(out_buffers.front());
					out_buffers.pop_front();
					while(buffer_item.buffer->size() > buffer_item.offset) {
						ssize_t result = write(fd, buffer_item.buffer->data() + buffer_item.offset, buffer_item.buffer->size() - buffer_item.offset);
						if(result < 0) {
							if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
								// try again later
								out_buffers.emplace_front(std::move(buffer_item));
								goto outerbreak;
							}

							if(errno != EPIPE) {
								fprintf(stderr, "Write to %s failed, %m. Closing.\n", fdinfos[fd].name.c_str());
							}
							close(fd);
							delpollfd(fd);
							continue_flag = false;
							break;
						}
						buffer_item.offset += result;
						fdinfos[fd].buffered_data -= result;
					}
					finished_with_buffer(std::move(buffer_item.buffer));
					outerbreak: break;
				}
			}
		}
	}
	cleanup();
	return 0;
}
