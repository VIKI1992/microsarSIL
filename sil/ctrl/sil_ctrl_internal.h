// sil_ctrl_internal.h - between the socket part (sil_ctrl_net.cpp) and the command part (sil_ctrl.cpp).
#ifndef SIL_CTRL_INTERNAL_H
#define SIL_CTRL_INTERNAL_H

#include <string>

/* Handles one request line (JSON object) and returns the reply (JSON object, no newline). */
std::string silctrl_handle_line(const std::string &line);
/* Called once when the server starts (registers the CAN sniffer). */
void silctrl_on_start();
/* Called after a reply was sent (performs a requested stop). */
void silctrl_after_reply();

#endif
