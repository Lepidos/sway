#include <stdlib.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include "log.h"
#include "sway/desktop/idle_inhibit_v1.h"
#include "sway/input/seat.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"
#include "sway/server.h"


static void destroy_inhibitor(struct sway_idle_inhibitor_v1 *inhibitor) {
	wl_list_remove(&inhibitor->link);
	wl_list_remove(&inhibitor->destroy.link);
	sway_idle_inhibit_v1_check_active();
	free(inhibitor);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_idle_inhibitor_v1 *inhibitor =
		wl_container_of(listener, inhibitor, destroy);
	sway_log(SWAY_DEBUG, "Sway idle inhibitor destroyed");
	destroy_inhibitor(inhibitor);
}

void handle_idle_inhibitor_v1(struct wl_listener *listener, void *data) {
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct sway_idle_inhibit_manager_v1 *manager =
		wl_container_of(listener, manager, new_idle_inhibitor_v1);
	sway_log(SWAY_DEBUG, "New sway idle inhibitor");

	struct sway_idle_inhibitor_v1 *inhibitor =
		calloc(1, sizeof(struct sway_idle_inhibitor_v1));
	if (!inhibitor) {
		return;
	}

	inhibitor->mode = INHIBIT_IDLE_APPLICATION;
	inhibitor->wlr_inhibitor = wlr_inhibitor;
	wl_list_insert(&manager->inhibitors, &inhibitor->link);

	inhibitor->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);

	sway_idle_inhibit_v1_check_active();
}

void handle_manager_destroy(struct wl_listener *listener, void *data) {
	struct sway_idle_inhibit_manager_v1 *manager =
		wl_container_of(listener, manager, manager_destroy);

	wl_list_remove(&manager->manager_destroy.link);
	wl_list_remove(&manager->new_idle_inhibitor_v1.link);
}

void sway_idle_inhibit_v1_user_inhibitor_register(struct sway_view *view,
		enum sway_idle_inhibit_mode mode) {
	struct sway_idle_inhibit_manager_v1 *manager = &server.idle_inhibit_manager_v1;

	struct sway_idle_inhibitor_v1 *inhibitor =
		calloc(1, sizeof(struct sway_idle_inhibitor_v1));
	if (!inhibitor) {
		return;
	}

	inhibitor->mode = mode;
	inhibitor->view = view;
	wl_list_insert(&manager->inhibitors, &inhibitor->link);

	inhibitor->destroy.notify = handle_destroy;
	wl_signal_add(&view->events.unmap, &inhibitor->destroy);

	sway_idle_inhibit_v1_check_active();
}

struct sway_idle_inhibitor_v1 *sway_idle_inhibit_v1_user_inhibitor_for_view(
		struct sway_view *view) {
	struct sway_idle_inhibit_manager_v1 *manager = &server.idle_inhibit_manager_v1;
	struct sway_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &manager->inhibitors, link) {
		if (inhibitor->mode != INHIBIT_IDLE_APPLICATION &&
				inhibitor->view == view) {
			return inhibitor;
		}
	}
	return NULL;
}

struct sway_idle_inhibitor_v1 *sway_idle_inhibit_v1_application_inhibitor_for_view(
		struct sway_view *view) {
	struct sway_idle_inhibit_manager_v1 *manager = &server.idle_inhibit_manager_v1;
	struct sway_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &manager->inhibitors, link) {
		if (inhibitor->mode == INHIBIT_IDLE_APPLICATION &&
				view_from_wlr_surface(inhibitor->wlr_inhibitor->surface) == view) {
			return inhibitor;
		}
	}
	return NULL;
}

void sway_idle_inhibit_v1_user_inhibitor_destroy(
		struct sway_idle_inhibitor_v1 *inhibitor) {
	if (!inhibitor) {
		return;
	}
	if (!sway_assert(inhibitor->mode != INHIBIT_IDLE_APPLICATION,
				"User should not be able to destroy application inhibitor")) {
		return;
	}
	destroy_inhibitor(inhibitor);
}

bool sway_idle_inhibit_v1_is_active(struct sway_idle_inhibitor_v1 *inhibitor) {
	if (server.session_lock.lock) {
		// A session lock is active. In this case, only application inhibitors
		// on the session lock surface can have any effect.
		if (inhibitor->mode != INHIBIT_IDLE_APPLICATION) {
			return false;
		}
		struct wlr_surface *wlr_surface = inhibitor->wlr_inhibitor->surface;
		if (!wlr_session_lock_surface_v1_try_from_wlr_surface(wlr_surface)) {
			return false;
		}
		return wlr_surface->mapped;
	}

	switch (inhibitor->mode) {
	case INHIBIT_IDLE_APPLICATION:;
		struct wlr_surface *wlr_surface = inhibitor->wlr_inhibitor->surface;
		struct wlr_layer_surface_v1 *layer_surface =
				wlr_layer_surface_v1_try_from_wlr_surface(wlr_surface);
		if (layer_surface) {
			// Layer surfaces can be occluded but are always on screen after
			// they have been mapped.
			return layer_surface->output && layer_surface->output->enabled &&
					wlr_surface->mapped;
		}

		// If there is no view associated with the inhibitor, assume invisible
		struct sway_view *view = view_from_wlr_surface(wlr_surface);
		return view && view->container && view_is_visible(view);
	case INHIBIT_IDLE_FOCUS:;
		struct sway_seat *seat = NULL;
		wl_list_for_each(seat, &server.input->seats, link) {
			struct sway_container *con = seat_get_focused_container(seat);
			if (con && con->view && con->view == inhibitor->view) {
				return true;
			}
		}
		return false;
	case INHIBIT_IDLE_FULLSCREEN:
		return inhibitor->view->container &&
			container_is_fullscreen_or_child(inhibitor->view->container) &&
			view_is_visible(inhibitor->view);
	case INHIBIT_IDLE_OPEN:
		// Inhibitor is destroyed on unmap so it must be open/mapped
		return true;
	case INHIBIT_IDLE_VISIBLE:
		return view_is_visible(inhibitor->view);
	}
	return false;
}

void sway_idle_inhibit_v1_check_active(void) {
	struct sway_idle_inhibit_manager_v1 *manager = &server.idle_inhibit_manager_v1;
	struct sway_idle_inhibitor_v1 *inhibitor;
	bool inhibited = false;
	wl_list_for_each(inhibitor, &manager->inhibitors, link) {
		if ((inhibited = sway_idle_inhibit_v1_is_active(inhibitor))) {
			break;
		}
	}
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier_v1, inhibited);
}

bool sway_idle_inhibit_manager_v1_init(void) {
	struct sway_idle_inhibit_manager_v1 *manager = &server.idle_inhibit_manager_v1;

	manager->wlr_manager = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!manager->wlr_manager) {
		return false;
	}

	wl_signal_add(&manager->wlr_manager->events.new_inhibitor,
		&manager->new_idle_inhibitor_v1);
	manager->new_idle_inhibitor_v1.notify = handle_idle_inhibitor_v1;
	wl_signal_add(&manager->wlr_manager->events.destroy,
		&manager->manager_destroy);
	manager->manager_destroy.notify = handle_manager_destroy;
	wl_list_init(&manager->inhibitors);

	return true;
}
