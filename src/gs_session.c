#include <assert.h>
#include "include/config.h"
#include "include/util.h"
#include "include/list.h"
#include "include/os_io.h"
#include "include/l2_string.h"
#include "include/gs_types.h"
#include "include/gs_client_packets.h"
#include "include/gs_session.h"

#define gs_session_each(session, state) \
        list_each(struct gs_session, session, state->list_sessions)

static size_t gs_session_get_free_id(struct gs_state *state)
{
        // Don't use id 0, it causes issues with packets
        // sent to the client.
        for (size_t i = 1, max = arr_size(state->sessions); i < max; i += 1) {
                if (!state->sessions[i].id) {
                        return i;
                }
        }

        return 0;
}

struct gs_session *gs_session_new(struct gs_state *state, struct os_io *socket)
{
        byte_t key[] = { 0x94, 0x35, 0x00, 0x00, 0xa1, 0x6c, 0x54, 0x87 };

        size_t id = 0;

        struct gs_session *new_session = 0;

        assert(socket);
        assert(state);

        id = gs_session_get_free_id(state);

        new_session = &state->sessions[id];

        bytes_zero((byte_t *) new_session, sizeof(*new_session));

        new_session->id     = (u32_t) id;
        new_session->socket = socket;

        bytes_cpy(new_session->encrypt_key, key, sizeof(key));
        bytes_cpy(new_session->decrypt_key, key, sizeof(key));

        list_add(state->list_sessions, new_session);

        return new_session;
}

struct gs_session *gs_session_find(struct gs_state *state, struct os_io *socket)
{
        struct gs_session *session = 0;

        assert(state);
        assert(socket);

        gs_session_each(session, state)
        {
                if (session->socket == socket) {
                        return session;
                }
        }

        return 0;
}

void gs_session_encrypt_conn(struct gs_session *session)
{
        assert(session);
        session->conn_encrypted = 1;
}

void gs_session_update_auth(
        struct gs_session *dest,
        struct gs_packet_auth_request *src)
{
        size_t max_username_size = 0;

        assert(dest);
        assert(src);

        max_username_size = sizeof(dest->username);
        l2_string_to_char(dest->username, src->username, max_username_size);

        dest->loginOK1 = src->loginOK1;
        dest->loginOK2 = src->loginOK2;
        dest->playOK1  = src->playOK1;
        dest->playOK2  = src->playOK2;
}

void gs_session_send_packet(
        struct gs_state *gs,
        struct gs_session *session,
        packet_t *src)
{
        assert(gs);
        assert(gs->send_response);
        assert(session);
        assert(session->socket);
        assert(src);
        gs->send_response(session->socket, src, (size_t) packet_size(src));
}

void gs_session_encrypt(struct gs_session *session, byte_t *dest, packet_t *src)
{
        packet_t *dest_body = 0;
        packet_t *src_body  = 0;

        u16_t src_size  = 0;
        u16_t body_size = 0;

        int temp  = 0;
        int temp2 = 0;

        unsigned int old = 0;

        assert(session);
        assert(dest);
        assert(src);

        src_size  = 2 + packet_padded_size(packet_size(src) - 2);
        body_size = src_size - 2;
        src_body  = packet_body(src);
        dest_body = packet_body(dest);

        for (u16_t i = 0; i < body_size; i++) {
                temp2 = src_body[i] & 0xff;
                dest_body[i] =
                        (byte_t)(temp2 ^ session->encrypt_key[i & 7] ^ temp);
                temp = dest_body[i];
        }

        old = ((unsigned int) session->encrypt_key[0] & 0xff);
        old |= ((unsigned int) session->encrypt_key[1] << 8 & 0xff00);
        old |= ((unsigned int) session->encrypt_key[2] << 0x10 & 0xff0000);
        old |= ((unsigned int) session->encrypt_key[3] << 0x18 & 0xff000000);

        old += body_size;

        session->encrypt_key[0] = (byte_t)(old & 0xff);
        session->encrypt_key[1] = (byte_t)(old >> 0x08 & 0xff);
        session->encrypt_key[2] = (byte_t)(old >> 0x10 & 0xff);
        session->encrypt_key[3] = (byte_t)(old >> 0x18 & 0xff);

        bytes_cpy(dest, (byte_t *) &src_size, 2);
}

void gs_session_decrypt(struct gs_session *session, packet_t *dest, byte_t *src)
{
        packet_t *dest_body = 0;
        packet_t *src_body  = 0;

        u16_t body_size = 0;

        int temp  = 0;
        int temp2 = 0;

        unsigned int old = 0;

        assert(session);
        assert(dest);
        assert(src);

        if (!session->conn_encrypted) {
                bytes_cpy(dest, src, (size_t) packet_size(src));
                return;
        }

        body_size = packet_size(src);
        body_size = body_size > 1 ? body_size - 2 : body_size;

        dest_body = packet_body(dest);
        src_body  = packet_body(src);

        for (u16_t i = 0; i < body_size; i++) {
                temp2 = src_body[i];
                dest_body[i] =
                        (byte_t)(temp2 ^ session->decrypt_key[i & 7] ^ temp);
                temp = temp2;
        }

        old = (unsigned int) session->decrypt_key[0] & 0xff;
        old |= (unsigned int) session->decrypt_key[1] << 8 & 0xff00;
        old |= (unsigned int) session->decrypt_key[2] << 0x10 & 0xff0000;
        old |= (unsigned int) session->decrypt_key[3] << 0x18 & 0xff000000;

        old += body_size;

        session->decrypt_key[0] = (byte_t)(old & 0xff);
        session->decrypt_key[1] = (byte_t)(old >> 0x08 & 0xff);
        session->decrypt_key[2] = (byte_t)(old >> 0x10 & 0xff);
        session->decrypt_key[3] = (byte_t)(old >> 0x18 & 0xff);

        bytes_cpy(dest, (byte_t *) &body_size, 2);
}

void gs_session_disconnected(struct gs_state *gs, struct gs_session *session)
{
        assert(gs);
        assert(session);

        list_remove(gs->list_sessions, session);
        *session = (struct gs_session){ 0 };
}

void gs_session_disconnect(struct gs_state *gs, struct gs_session *session)
{
        assert(gs);
        assert(gs->disconnect);
        assert(session);
        assert(session->socket);

        gs->disconnect(session->socket);
        gs_session_disconnected(gs, session);
}
