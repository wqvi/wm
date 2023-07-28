#ifndef LISTENERS_H
#define LISTENERS_H

void motionabsolute(struct wl_listener *listener, void *data);

void virtualkeyboard(struct wl_listener *listener, void *data);

void urgent(struct wl_listener *listener, void *data);

void setpsel(struct wl_listener *listener, void *data);

void setsel(struct wl_listener *listener, void *data);

void setcursor(struct wl_listener *listener, void *data);

void outputmgrtest(struct wl_listener *listener, void *data);

void outputmgrapply(struct wl_listener *listener, void *data);

void locksession(struct wl_listener *listener, void *data);

void inputdevice(struct wl_listener *listener, void *data);

void createnotify(struct wl_listener *listener, void *data);

void createmon(struct wl_listener *listener, void *data);

void createlayersurface(struct wl_listener *listener, void *data);

void createdecoration(struct wl_listener *listener, void *data);

void unmaplayersurfacenotify(struct wl_listener *listener, void *data);

void unmapnotify(struct wl_listener *listener, void *data);

void updatemons(struct wl_listener *listener, void *data);

void updatetitle(struct wl_listener *listener, void *data);

void keypress(struct wl_listener *listener, void *data);

void keypressmod(struct wl_listener *listener, void *data);

void cleanupkeyboard(struct wl_listener *listener, void *data);

void cleanupmon(struct wl_listener *listener, void *data);

void commitlayersurfacenotify(struct wl_listener *listener, void *data);

void commitnotify(struct wl_listener *listener, void *data);

void createlocksurface(struct wl_listener *listener, void *data);

void fullscreennotify(struct wl_listener *listener, void *data);

void maplayersurfacenotify(struct wl_listener *listener, void *data);

void mapnotify(struct wl_listener *listener, void *data);

void rendermon(struct wl_listener *listener, void *data);

void unlocksession(struct wl_listener *listener, void *data);

#endif // LISTENERS_H
