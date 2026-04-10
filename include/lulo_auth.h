#ifndef LULO_AUTH_H
#define LULO_AUTH_H

#include "lulo_app.h"

int lulo_auth_start(AppState *app);
void lulo_auth_cancel(AppState *app);
void lulo_auth_poll(AppState *app, RenderFlags *render);
void lulo_auth_append_text(AppState *app, const char *text, size_t len);
void lulo_auth_backspace(AppState *app);
void lulo_auth_submit(AppState *app);
void lulo_auth_shutdown(AppState *app);

#endif
