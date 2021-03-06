/*
 * Daemon for controlling Clear Linux Software Update Client
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * Contact: Dmitry Rozhkov <dmitry.rozhkov@intel.com>
 *
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>

#include "log.h"
#include "list.h"

#define SWUPD_CLIENT    "swupd"
#define TIMEOUT_EXIT_SEC 30

typedef enum {
	METHOD_NOTSET = 0,
	METHOD_CHECK_UPDATE,
	METHOD_UPDATE,
	METHOD_VERIFY,
	METHOD_BUNDLE_ADD,
	METHOD_BUNDLE_REMOVE
} method_t;

typedef struct _daemon_state {
	sd_bus *bus;
	pid_t child;
	method_t method;
} daemon_state_t;

static const char * const _method_str_map[] = {
	NULL,
	"checkUpdate",
	"update",
	"verify",
	"bundleAdd",
	"bundleRemove"
};

static const char * const _method_opt_map[] = {
	NULL,
	"check-update",
	"update",
	"verify",
	"bundle-add",
	"bundle-remove"
};

static char **list_to_strv(struct list *strlist)
{
	char **strv;
	char **temp;

	strv = (char **)malloc((list_len(strlist) + 1) * sizeof(char *));
	memset(strv, 0x00, (list_len(strlist) + 1) * sizeof(char *));

	temp = strv;
	while (strlist)
	{
		*temp = strlist->data;
		temp++;
		strlist = strlist->next;
	}
	return strv;
}

static int is_in_array(const char *key, char const * const arr[])
{
	if (arr == NULL) {
		return 0;
	}

	char const * const *temp = arr;
	while (*temp) {
		if (strcmp(key, *temp) == 0) {
			return 1;
		}
		temp++;
	}
	return 0;
}

static int bus_message_read_option_string(struct list **strlist,
					  sd_bus_message *m,
					  const char *optname)
{
	int r = 0;
	char *option = NULL;
	const char *value;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_read(m, "s", &value);
	if (r < 0) {
		ERR("Can't read option value: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit variant container: %s", strerror(-r));
		return r;
	}

	r = asprintf(&option, "--%s", optname);
	if (r < 0) {
		return -ENOMEM;
	}

	*strlist = list_append_data(*strlist, option);
	*strlist = list_append_data(*strlist, strdup(value));

	return 0;
}

static int bus_message_read_option_bool(struct list **strlist,
					  sd_bus_message *m,
					  const char *optname)
{
	int value;
	int r;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_read(m, "b", &value);
	if (r < 0) {
		ERR("Can't read option value: %s", strerror(-r));
		return r;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit variant container: %s", strerror(-r));
		return r;
	}

	if (value) {
		char *option = NULL;

		r = asprintf(&option, "--%s", optname);
		if (r < 0) {
			return -ENOMEM;
		}
		*strlist = list_append_data(*strlist, option);
	}
	return 0;
}

static int bus_message_read_options(struct list **strlist,
	                            sd_bus_message *m,
	                            char const * const opts_str[],
	                            char const * const opts_bool[])
{
	int r;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
	if (r < 0) {
		ERR("Failed to enter array container: %s", strerror(-r));
		return r;
	}

	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const char *argname;

                r = sd_bus_message_read(m, "s", &argname);
                if (r < 0) {
			ERR("Can't read argument name: %s", strerror(-r));
                        return r;
		}
		if (is_in_array(argname, opts_str)) {
			r = bus_message_read_option_string(strlist, m, argname);
			if (r < 0) {
				ERR("Can't read option '%s': %s", argname, strerror(-r));
				return r;
			}
		} else if (is_in_array(argname, opts_bool)) {
			r = bus_message_read_option_bool(strlist, m, argname);
			if (r < 0) {
				ERR("Can't read option '%s': %s", argname, strerror(-r));
				return r;
			}
		} else {
			r = sd_bus_message_skip(m, "v");
			if (r < 0) {
				ERR("Can't skip unwanted option value: %s", strerror(-r));
				return r;
			}
		}

                r = sd_bus_message_exit_container(m);
                if (r < 0) {
			ERR("Can't exit dict entry container: %s", strerror(-r));
                        return r;
		}
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit array container: %s", strerror(-r));
		return r;
	}

	return 0;
}

static int on_name_owner_change(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	sd_event *e = userdata;

	assert(m);
	assert(e);

	sd_bus_close(sd_bus_message_get_bus(m));
	sd_event_exit(e, 0);

	return 1;
}

static int on_childs_output(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	daemon_state_t *context = userdata;
	int r = 0;
	char buffer[PIPE_BUF + 1];
	ssize_t count;
	count = read(fd, buffer, PIPE_BUF);
	if (count > 0) {
		fwrite(buffer, 1, count, stdout);
		fflush(stdout);
		buffer[count] = '\0';
		r = sd_bus_emit_signal(context->bus,
				       "/org/O1/swupdd/Client",
				       "org.O1.swupdd.Client",
				       "childOutputReceived", "s", buffer);
		if (r < 0) {
			ERR("Failed to emit signal: %s", strerror(-r));
		}
	} else if (count < 0) {
		r = -errno;
		ERR("Failed to read pipe: %s", strerror(errno));
	} else {
		close(fd);
		sd_event_source_unref(s);
	}

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int on_child_exit(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata)
{
	daemon_state_t *context = userdata;
	int child_exit_status;
	int status = 0;
	int r;

	method_t child_method = context->method;
	assert(child_method);

	if (si->ssi_code == CLD_EXITED) {
		status = si->ssi_status;
	} else {
		DEBUG("Child process was killed");
		/* si->ssi_status is signal code in this case */
		status = 128 + si->ssi_status;
	}

	/* Reap the zomby */
	assert(si->ssi_pid == context->child);
	pid_t ws = waitpid(si->ssi_pid, &child_exit_status, 0);
	assert(ws != -1);

	context->child = 0;
	context->method = METHOD_NOTSET;

	r = sd_bus_emit_signal(context->bus,
			       "/org/O1/swupdd/Client",
			       "org.O1.swupdd.Client",
			       "requestCompleted", "si", _method_str_map[child_method], status);
	if (r < 0) {
		ERR("Can't emit D-Bus signal: %s", strerror(-r));
		goto finish;
	}

finish:
	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int run_swupd(method_t method, struct list *args, daemon_state_t *context)
{
	pid_t pid;
	int fds[2];
	int r;

	r = pipe2(fds, O_DIRECT);
	if (r < 0) {
		ERR("Can't create pipe: %s", strerror(errno));
		return -errno;
	}

	pid = fork();

	if (pid == 0) { /* child */
		dup2(STDOUT_FILENO, STDERR_FILENO);
		while ((dup2(fds[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
		close(fds[1]);
		close(fds[0]);
		char **argv = list_to_strv(list_head(args));
		execvp(*argv, argv);
		ERR("This line must not be reached");
		_exit(1);
	} else if (pid < 0) {
		ERR("Failed to fork: %s", strerror(errno));
		return -errno;
	}

	close(fds[1]);
	context->child = pid;
	context->method = method;
	sd_event *event = NULL;
	r = sd_event_default(&event);
	r = sd_event_add_io(event, NULL, fds[0], EPOLLIN, on_childs_output, context);
	assert(r >= 0);
	sd_event_unref(event);

	return 0;
}

static int method_update(sd_bus_message *m,
	                 void *userdata,
	                 sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	struct list *args = NULL;

	if (context->child) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_UPDATE]));

	char const * const accepted_str_opts[] = {"url", "contenturl", "versionurl", "log", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_UPDATE, args, context);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_verify(sd_bus_message *m,
	                 void *userdata,
	                 sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	struct list *args = NULL;

	if (context->child) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_VERIFY]));

	char const * const accepted_str_opts[] = {"url", "contenturl", "versionurl", "log", NULL};
	char const * const accepted_bool_opts[] = {"fix", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, accepted_bool_opts);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_VERIFY, args, context);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_check_update(sd_bus_message *m,
			       void *userdata,
			       sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	struct list *args = NULL;

	if (context->child) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_CHECK_UPDATE]));

	char const * const accepted_str_opts[] = {"url", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	const char *bundle;
	r = sd_bus_message_read(m, "s", &bundle);
	if (r < 0) {
		ERR("Can't read bundle: %s", strerror(-r));
		goto finish;
	}
	args = list_append_data(args, strdup(bundle));

	r  = run_swupd(METHOD_CHECK_UPDATE, args, context);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_bundle_add(sd_bus_message *m,
			     void *userdata,
			     sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	struct list *args = NULL;
	const char* bundle = NULL;

	if (context->child) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_BUNDLE_ADD]));

	char const * const accepted_str_opts[] = {"url", NULL};
	char const * const accepted_bool_opts[] = {"list", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, accepted_bool_opts);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s");
	if (r < 0) {
		ERR("Can't enter container: %s", strerror(-r));
		goto finish;
	}
	while ((r = sd_bus_message_read(m, "s", &bundle)) > 0) {
		args = list_append_data(args, strdup(bundle));
	}
	if (r < 0) {
		ERR("Can't read bundle name from message: %s", strerror(-r));
		goto finish;
	}
	r = sd_bus_message_exit_container(m);
	if (r < 0) {
		ERR("Can't exit array container: %s", strerror(-r));
		goto finish;
	}

	r  = run_swupd(METHOD_BUNDLE_ADD, args, context);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_bundle_remove(sd_bus_message *m,
				void *userdata,
				sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	struct list *args = NULL;

	if (context->child) {
		r = -EAGAIN;
		ERR("Busy with ongoing request to swupd");
		goto finish;
	}

	args = list_append_data(args, strdup(SWUPD_CLIENT));
	args = list_append_data(args, strdup(_method_opt_map[METHOD_BUNDLE_REMOVE]));

	char const * const accepted_str_opts[] = {"url", NULL};
	r = bus_message_read_options(&args, m, accepted_str_opts, NULL);
	if (r < 0) {
		ERR("Can't read options: %s", strerror(-r));
		goto finish;
	}

	const char *bundle;
	r = sd_bus_message_read(m, "s", &bundle);
	if (r < 0) {
		ERR("Can't read bundle: %s", strerror(-r));
		goto finish;
	}
	args = list_append_data(args, strdup(bundle));

	r  = run_swupd(METHOD_BUNDLE_REMOVE, args, context);
	if (r < 0) {
		ERR("Got error when running swupd command: %s", strerror(-r));
	}

finish:
	list_free_list_and_data(args, free);
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

static int method_cancel(sd_bus_message *m,
			 void *userdata,
			 sd_bus_error *ret_error)
{
	daemon_state_t *context = userdata;
	int r = 0;
	pid_t child;
	int force;

	child = context->child;
	if (!child) {
		r = -ECHILD;
		ERR("No child process to cancel");
		goto finish;
	}

	r = sd_bus_message_read(m, "b", &force);
	if (r < 0) {
		ERR("Can't read 'force' option: %s", strerror(-r));
		goto finish;
	}

	if (force) {
		kill(child, SIGKILL);
	} else {
		kill(child, SIGINT);
	}

finish:
	return sd_bus_reply_method_return(m, "b", (r >= 0));
}

/* Basically this is a copypasta from systemd's internal function bus_event_loop_with_idle() */
static int run_bus_event_loop(sd_event *event,
			      daemon_state_t *context)
{
	sd_bus *bus = context->bus;
	bool exiting = false;
	int r, code;

	for (;;) {
		r = sd_event_get_state(event);
		if (r < 0) {
			ERR("Failed to get event loop's state: %s", strerror(-r));
			return r;
		}
		if (r == SD_EVENT_FINISHED) {
			break;
		}

		r = sd_event_run(event, exiting ? (uint64_t) -1 : (uint64_t) TIMEOUT_EXIT_SEC * 1000000);
		if (r < 0) {
			ERR("Failed to run event loop: %s", strerror(-r));
			return r;
		}

		if (!context->method && r == 0 && !exiting) {
			r = sd_bus_try_close(bus);
			if (r == -EBUSY) {
				continue;
			}
                        /* Fallback for dbus1 connections: we
                         * unregister the name and wait for the
                         * response to come through for it */
                        if (r == -EOPNOTSUPP) {
				sd_notify(false, "STOPPING=1");

				/* We unregister the name here and then wait for the
				 * NameOwnerChanged signal for this event to arrive before we
				 * quit. We do this in order to make sure that any queued
				 * requests are still processed before we really exit. */

				char *match = NULL;
				const char *unique;
				r = sd_bus_get_unique_name(bus, &unique);
				if (r < 0) {
					ERR("Failed to get unique D-Bus name: %s", strerror(-r));
					return r;
				}
				r = asprintf(&match,
					     "sender='org.freedesktop.DBus',"
					     "type='signal',"
					     "interface='org.freedesktop.DBus',"
					     "member='NameOwnerChanged',"
					     "path='/org/freedesktop/DBus',"
					     "arg0='org.O1.swupdd.Client',"
					     "arg1='%s',"
					     "arg2=''", unique);
				if (r < 0) {
					ERR("Not enough memory to allocate string");
					return r;
				}

				r = sd_bus_add_match(bus, NULL, match, on_name_owner_change, event);
				if (r < 0) {
					ERR("Failed to add signal listener: %s", strerror(-r));
					free(match);
					return r;
				}

				r = sd_bus_release_name(bus, "org.O1.swupdd.Client");
				if (r < 0) {
					ERR("Failed to release service name: %s", strerror(-r));
					free(match);
					return r;
				}

				exiting = true;
				free(match);
				continue;
			}

			if (r < 0) {
				ERR("Failed to close bus: %s", strerror(-r));
				return r;
			}
			sd_event_exit(event, 0);
			break;
		}
	}

        r = sd_event_get_exit_code(event, &code);
        if (r < 0) {
                return r;
	}

        return code;
}

static const sd_bus_vtable swupdd_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("checkUpdate", "a{sv}s", "b", method_check_update, 0),
	SD_BUS_METHOD("update", "a{sv}", "b", method_update, 0),
	SD_BUS_METHOD("verify", "a{sv}", "b", method_verify, 0),
	SD_BUS_METHOD("bundleAdd", "a{sv}as", "b", method_bundle_add, 0),
	SD_BUS_METHOD("bundleRemove", "a{sv}s", "b", method_bundle_remove, 0),
	SD_BUS_METHOD("cancel", "b", "b", method_cancel, 0),
	SD_BUS_SIGNAL("requestCompleted", "si", 0),
	SD_BUS_SIGNAL("childOutputReceived", "s", 0),
	SD_BUS_VTABLE_END
};

int main(int argc, char *argv[]) {
	daemon_state_t context = {};
	sd_bus_slot *slot = NULL;
	sd_event *event = NULL;
	sigset_t ss;
	int r;

	memset(&context, 0x00, sizeof(daemon_state_t));

        r = sd_event_default(&event);
        if (r < 0) {
                ERR("Failed to allocate event loop: %s", strerror(-r));
                goto finish;
        }

	if (sigemptyset(&ss) < 0 ||
	    sigaddset(&ss, SIGCHLD) < 0) {
		r = -errno;
		goto finish;
	}

	if (sigprocmask(SIG_BLOCK, &ss, NULL) < 0) {
		ERR("Failed to set signal proc mask: %s", strerror(errno));
		goto finish;
	}
	r = sd_event_add_signal(event, NULL, SIGCHLD, on_child_exit, &context);
	if (r < 0) {
		ERR("Failed to add signal: %s", strerror(-r));
		goto finish;
	}

        sd_event_set_watchdog(event, true);

	r = sd_bus_open_system(&context.bus);
	if (r < 0) {
		ERR("Failed to connect to system bus: %s", strerror(-r));
		goto finish;
	}

	r = sd_bus_add_object_vtable(context.bus,
				     &slot,
				     "/org/O1/swupdd/Client",
				     "org.O1.swupdd.Client",
				     swupdd_vtable,
				     NULL);
	if (r < 0) {
		ERR("Failed to issue method call: %s", strerror(-r));
		goto finish;
	}

	sd_bus_slot_set_userdata(slot, &context);

	r = sd_bus_request_name(context.bus, "org.O1.swupdd.Client", 0);
	if (r < 0) {
		ERR("Failed to acquire service name: %s", strerror(-r));
		goto finish;
	}

        r = sd_bus_attach_event(context.bus, event, 0);
        if (r < 0) {
                ERR("Failed to attach bus to event loop: %s", strerror(-r));
		goto finish;
	}

	sd_notify(false,
		  "READY=1\n"
		  "STATUS=Daemon startup completed, processing events.");
	r = run_bus_event_loop(event, &context);

finish:
	sd_bus_slot_unref(slot);
	sd_bus_unref(context.bus);
	sd_event_unref(event);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
