/* SPDX-License-Identifier: MIT
 * Test helper: create a window, print its id, then keep the connection
 * open until stdin closes. */
#include <stdio.h>
#include <xcb/xcb.h>

int
main(void)
{
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c))
        return 1;
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    xcb_create_window(c, 0, w, s->root, 0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      s->root_visual, 0, NULL);
    xcb_flush(c);
    xcb_get_input_focus_reply_t *r = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL);
    (void) r;
    printf("0x%x\n", w);
    fflush(stdout);
    while (getchar() != EOF)
        ;
    xcb_disconnect(c);
    return 0;
}
