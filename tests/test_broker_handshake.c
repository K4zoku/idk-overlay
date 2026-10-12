#include <string.h>

#include "../src/cli/broker/internal.h"
#include "test_runner.h"

static idk_cp_handshake_t valid_handshake(void) {
    idk_cp_handshake_t hs = {0};
    hs.identity = IDK_CP_ID_OVERLAY;
    strcpy(hs.comm, "game.exe");
    strcpy(hs.tp_socket, "idk_tp_1");
    strcpy(hs.input_socket, "idk_input_1");
    return hs;
}

TEST(accepts_valid_handshake) {
    idk_cp_handshake_t hs = valid_handshake();
    ASSERT_EQ(validate_handshake(&hs), 0);
}

TEST(rejects_unterminated_comm) {
    idk_cp_handshake_t hs = valid_handshake();
    memset(hs.comm, 'A', sizeof(hs.comm));
    ASSERT_EQ(validate_handshake(&hs), -1);
}

TEST(rejects_unterminated_webview) {
    idk_cp_handshake_t hs = valid_handshake();
    memset(hs.webview, 'B', sizeof(hs.webview));
    ASSERT_EQ(validate_handshake(&hs), -1);
}

int main(void) {
    RUN(accepts_valid_handshake);
    RUN(rejects_unterminated_comm);
    RUN(rejects_unterminated_webview);
    return 0;
}
