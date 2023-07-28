#ifndef CLIENT_H

void client_get_size_hints(struct Client *c, struct wlr_box *max, struct wlr_box *min);

struct wlr_surface *client_surface(struct Client *c);

int toplevel_from_wlr_surface(struct wlr_surface *s, struct Client **pc, struct LayerSurface **pl);

void client_activate_surface(struct wlr_surface *s, int activated);

uint32_t client_set_bounds(struct Client *c, int32_t width, int32_t height);

void client_for_each_surface(struct Client *c, wlr_surface_iterator_func_t fn, void *data);

const char *client_get_appid(struct Client *c);

void client_get_geometry(struct Client *c, struct wlr_box *geom);

struct Client *client_get_parent(struct Client *c);

const char *client_get_title(struct Client *c);

int client_is_float_type(struct Client *c);

int client_is_mapped(struct Client *c);

int client_is_rendered_on_mon(struct Client *c, struct Monitor *m);

int client_is_stopped(struct Client *c);

void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb);

void client_restack_surface(struct Client *c);

void client_send_close(struct Client *c);

void client_set_fullscreen(struct Client *c, int fullscreen);

uint32_t client_set_size(struct Client *c, uint32_t width, uint32_t height);

void client_set_tiled(struct Client *c, uint32_t edges);

struct wlr_surface *client_surface_at(struct Client *c, double cx, double cy, double *sx, double *sy);

int client_wants_focus(struct Client *c);

int client_wants_fullscreen(struct Client *c);

#endif // CLIENT_H
