#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <getopt.h>
#include <errno.h>
#include <openssl/evp.h>
#include <libnotify/notify.h>

#define DEBUG    0
#define MAXBUF   1024
#define TOK      "/"
#define FMTCALL  "  -!-  Call from %s"
#define FMTOTHER "  -!-  %s"
#define AES_BLOCK_SIZE 128
#define SEP    "/"

#define STREQ(a, b)     strcmp((a),(b)) == 0
#define STRDUP(a, b)    if ((b)) a = strdup((b))

static int  portno = 10600;

enum etype {
    Ring,
    SMS,
    MMS,
    Battery,
    Ping,
    Unknown
};

struct message_t {
    int         version;
    char *      device_id;
    char *      notification_id;
    enum etype  event_type;
    char *      data;
    char *      event_contents;
};

static int parse_options(int argc, char *argv[]) {
    int opt, option_index = 0;
    char *token;

    static struct option opts[] = {
        { "port"   , required_argument, 0, 'p'},
        { "help"   , no_argument,       0, 'h'},
        { 0        , 0                , 0, 0  }
    };

    while ((opt = getopt_long(argc, argv, "hp:", opts, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                portno = strtol(optarg, &token, 10);
                if (*token != '\0' || portno <= 0 || portno > 65535) {
                    fprintf(stderr, "invalid port number\n");
                    exit(EXIT_FAILURE);
                }
                break;

            case 'h':
                printf("usage: android-receiver [ -p <port> ]\n");
                exit(EXIT_SUCCESS);

            default:
                fprintf(stderr, "invalid option, try -h or --help\n");
                exit(EXIT_FAILURE);
        }
    }

    return 0;
}
/* }}} */

static struct message_t *parse_message(char *msg) {
    struct message_t *message;
    char *tmp, *tok;
    int field = 0;

    message = malloc(sizeof *message);

    /* v1:        device_id / notification_id / event_type /        event_contents */
    /* v2: "v2" / device_id / notification_id / event_type / data / event_contents */
    for (tok = strsep(&msg, SEP); ++field <= 5; tok = strsep(&msg, SEP)) {
        switch (field) {
            case 1:
                if (tok && STREQ(tok, "v2"))
                    message->version = 2;
                else {
                    /* rebuild a v1 msg to start parsing at field 2 */
                    message->version = 1;
                    tmp = strdup(msg);
                    strcat(tok, TOK);
                    strcat(tok, tmp);
                    msg = tok;
                }
                break;

            case 2: STRDUP(message->device_id, tok);       break;
            case 3: STRDUP(message->notification_id, tok); break;

            case 4:
                if      (STREQ(tok, "RING"))    message->event_type = Ring;
                else if (STREQ(tok, "SMS"))     message->event_type = SMS;
                else if (STREQ(tok, "MMS"))     message->event_type = MMS;
                else if (STREQ(tok, "BATTERY")) message->event_type = Battery;
                else if (STREQ(tok, "PING"))    message->event_type = Ping;
                else                            message->event_type = Unknown;

                if (message->version == 1) {
                    /* for v1, grab everything else and return */
                    STRDUP(message->event_contents, msg);
                    message->data = "";
                    return message;
                }
                break;

            case 5:
                STRDUP(message->data, tok);
                STRDUP(message->event_contents, msg);
                return message;
        }
    }

    return message;
}
/* }}} */

static void handle_message(char *msg) {
    printf("message received: %s\n", msg);

    struct message_t *message;
    char *title;
    char *icon;

    message = parse_message(msg);

    switch (message->event_type) {
        case Ring:
            asprintf(&title, "Call: %s", message->data);
            icon = "call-start";
            break;

        case SMS:
            asprintf(&title, "SMS: %s", message->data);
            icon = "stock_mail-unread";
            break;
        case MMS:
            asprintf(&title, "MMS: %s", message->data);
            icon = "stock_mail-unread";
            break;
        case Battery:
            asprintf(&title, "Battery: %s", message->data);
            icon = "battery-good"; // TODO: various levels and charge-state
            break;
        case Ping:
            title = "Ping";
            icon = "emblem-important";
            break;

        default:
            return;
    }

    notify_init("android-receiver");
    NotifyNotification *n = notify_notification_new(title, message->event_contents, icon);
    notify_notification_show(n, NULL);
    g_object_unref(G_OBJECT(n));
    notify_uninit();
}
/* }}} */

static int key_init(unsigned char *key_data, int key_data_len, EVP_CIPHER_CTX *ctx) {
    int i, nrounds = 10;
    unsigned char key[16], iv[16];
    i = EVP_BytesToKey(EVP_aes_128_cbc(), EVP_md5(), NULL, key_data, key_data_len, nrounds, key, iv);
    printf("Key is %d bytes\n",i);
    EVP_CIPHER_CTX_init(ctx);
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv);

    return 0;
}

static unsigned char *decrypt(EVP_CIPHER_CTX *ctx, unsigned char *ciphertext, int *len){
    int p_len = *len, f_len = 0;
    unsigned char *plaintext = malloc(p_len + AES_BLOCK_SIZE);
    
    EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, NULL);
    EVP_DecryptUpdate(ctx, plaintext, &p_len, ciphertext, *len);
    EVP_DecryptFinal_ex(ctx, plaintext+p_len, &f_len);

    *len = p_len + f_len;
    return plaintext;
}

int main(int argc, char *argv[]) { /* {{{ */
    char buf[MAXBUF];

    char *plain;
    unsigned char *ubuf = (unsigned char *)buf;

    struct sockaddr_in server, from;

    EVP_CIPHER_CTX ctx;

    int          sock, n;
    unsigned int fromlen = sizeof from;

    unsigned char *key_data = (unsigned char*)"passphrase";
    int key_data_len = strlen("passphrase");
    int len = 1024;

    if(key_init(key_data, key_data_len, &ctx)){
        printf("Key creation failed\n");
        exit(EXIT_FAILURE);
    }

    parse_options(argc, argv);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("error opening socket");
        exit(EXIT_FAILURE);
    }

    memset(&server, '\0', sizeof server);

    server.sin_family      = AF_INET;
    server.sin_port        = htons(portno);
    server.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&server, sizeof server) < 0) {
        perror("error binding to socket");
        exit(EXIT_FAILURE);
    }

    while (1) {
        while ((n = recvfrom(sock, buf, 1024, 0, (struct sockaddr *)&from, &fromlen)) < 0 && errno == EINTR)
            ;

        if (n < 0) {
            perror("error receiving from socket");
            exit(EXIT_FAILURE);
        }

        len=strlen(buf)+1;
        plain=(char *)decrypt(&ctx,ubuf,&len);

        handle_message(plain);
    }
    EVP_CIPHER_CTX_cleanup(&ctx);
    free(plain);
    free(ubuf);
    free(key_data);
}
/* }}} */
