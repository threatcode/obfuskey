#define _GNU_SOURCE
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <stdbool.h>
#include <sodium.h>
#include <sys/queue.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include "obfuskey.h"
#include "keycodes.h"

#define BUFSIZE 256                  // for device names and rescue key sequence
#define MAX_INPUTS 48                // number of devices to try autodetection
#define MAX_DEVICES 48               // max number of devices to read events from
#define MAX_RESCUE_KEYS 10           // max number of rescue keys to exit in case of emergency
#define MIN_KEYBOARD_KEYS 20         // need at least this many keys to be a keyboard
#define POLL_TIMEOUT_MS 1            // timeout to check for new events
#define DEFAULT_MAX_DELAY_MS 100      // upper bound on event delay
#define DEFAULT_STARTUP_DELAY_MS 500 // wait before grabbing the input device

#define panic(format, ...) do { fprintf(stderr, format "\n", ## __VA_ARGS__); fflush(stderr); cleanup(); exit(EXIT_FAILURE); } while (0)

#ifndef min
#define min(a, b) ( ((a) < (b)) ? (a) : (b) )
#endif

#ifndef max
#define max(a, b) ( ((a) > (b)) ? (a) : (b) )
#endif

const char *qubes_detect_file = "/usr/share/qubes/marker-vm";
static bool is_qubes_vm = false;

static int interrupt = 0;       // flag to interrupt the main loop and exit
static int verbose = 0;         // flag for verbose output
static int persistent = 0;      // flag for persistent mode (diables rescue key sequence)
static int custom_rescue = 0;   // flag for setting a custom rescue key sequence

static char rescue_key_seps[] = ", ";  // delims to strtok
static char rescue_keys_str[BUFSIZE] = "KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_ESC";
static int rescue_keys[MAX_RESCUE_KEYS];  // Codes of the rescue key combo
static int rescue_len = 0;      // Number of rescue keys, set during initialization

static int max_delay = DEFAULT_MAX_DELAY_MS;  // lag will never exceed this upper bound
static int startup_timeout = DEFAULT_STARTUP_DELAY_MS;

static unsigned int device_count = 0;
static char named_inputs[MAX_INPUTS][BUFSIZE];

static int input_fds[MAX_INPUTS];
struct libevdev *output_devs[MAX_INPUTS];
struct libevdev_uinput *uidevs[MAX_INPUTS];

static struct option long_options[] = {
    {"read",    1, 0, 'r'},
    {"delay",   1, 0, 'd'},
    {"start",   1, 0, 's'},
    {"keys",    1, 0, 'k'},
    {"verbose", 0, 0, 'v'},
    {"help",    0, 0, 'h'},
    {0,         0, 0, 0}
};

TAILQ_HEAD(tailhead, entry) head;

// From string_copying manpage
ssize_t strtcpy(char *restrict dst, const char *restrict src, size_t dsize)
{
    bool trunc;
    size_t dlen, slen;

    if (dsize == 0) {
        errno = ENOBUFS;
        return -1;
    }

    slen = strnlen(src, dsize);
    trunc = (slen == dsize);
    dlen = slen - trunc;

    stpcpy(mempcpy(dst, src, dlen), "");
    if (trunc)
        errno = E2BIG;
    return trunc ? -1 : (ssize_t)slen;
}

void cleanup() {
    for (int i = 0; i < device_count; i++) {
        libevdev_uinput_destroy(uidevs[i]);
        libevdev_free(output_devs[i]);
        close(input_fds[i]);
    }
}

void sleep_ms(long milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    if (nanosleep(&ts, NULL) == -1)
      panic("nanosleep failed: %s", strerror(errno));
}

long current_time_ms(void) {
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (spec.tv_sec) * 1000 + (spec.tv_nsec) / 1000000;
}

long random_between(long lower, long upper) {
    long maxval;
    long randval;
    // default to max if the interval is not valid
    if (lower >= upper)
        return upper;

    maxval = upper - lower + 1;
    if (maxval > UINT32_MAX)
        return UINT32_MAX;

    randval = randombytes_uniform((uint32_t)maxval);
    return lower + randval;
}

void set_rescue_keys(const char* rescue_keys_str) {
    char* _rescue_keys_str = malloc(strlen(rescue_keys_str) + 1);
    if(_rescue_keys_str == NULL) {
        panic("Failed to allocate memory for _rescue_keys_str");
    }
    strncpy(_rescue_keys_str, rescue_keys_str, strlen(rescue_keys_str));
    _rescue_keys_str[strlen(rescue_keys_str)] = '\0';

    char* token = strtok(_rescue_keys_str, rescue_key_seps);

    while (token != NULL) {
        int keycode = lookup_keycode(token);
        if (keycode < 0) {
            panic("Invalid key name: '%s'\nSee keycodes.h for valid names", token);
        } else if (rescue_len < MAX_RESCUE_KEYS) {
            rescue_keys[rescue_len] = keycode;
            rescue_len++;
        } else {
            panic("Cannot set more than %d rescue keys", MAX_RESCUE_KEYS);
        }
        token = strtok(NULL, rescue_key_seps);
    }
    free(_rescue_keys_str);
}

int supports_event_type(int device_fd, int event_type) {
    unsigned long evbit = 0;
    // Get the bit field of available event types.
    if (ioctl(device_fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) == -1)
      panic("ioctl EVIOCGBIT failed: %s", strerror(errno));
    // NOTE: EVIOCGBIT ioctl returns an int, see handle_eviocgbit function in
    // linux/drivers/input/evdev.c, thus this cast is safe
    return (int)evbit & (1 << event_type);
}

int supports_specific_key(int device_fd, unsigned int key) {
    size_t nchar = KEY_MAX/8 + 1;
    unsigned char bits[nchar];
    // Get the bit fields of available keys.
    if (ioctl(device_fd, EVIOCGBIT(EV_KEY, sizeof(bits)), &bits) == -1)
      panic("ioctl EVIOCGBIT for EV_KEY failed: %s", strerror(errno));
    return bits[key/8] & (1 << (key % 8));
}

int is_keyboard(int fd) {
    unsigned int key;
    int num_supported_keys = 0;

    // Only check devices that support EV_KEY events
    if (supports_event_type(fd, EV_KEY)) {
        // Count the number of KEY_* events that are supported
        for (key = 0; key <= KEY_MAX; key++) {
            if (supports_specific_key(fd, key)) {
                num_supported_keys += 1;
            }
        }
    }

    return (num_supported_keys > MIN_KEYBOARD_KEYS);
}

int is_mouse(int fd) {
    return (supports_event_type(fd, EV_REL) || supports_event_type(fd, EV_ABS));
}

void detect_devices() {
    int fd;
    char device[BUFSIZE];

    for (int i = 0; i < MAX_DEVICES; i++) {
        snprintf(device, sizeof(device), "/dev/input/event%d", i);

        if ((fd = open(device, O_RDONLY)) < 0) {
            continue;
        }

        if (is_keyboard(fd)) {
            strtcpy(named_inputs[device_count++], device, BUFSIZE);
            if (verbose)
                printf("Found keyboard at: %s\n", device);
        } else if (is_mouse(fd)) {
            strtcpy(named_inputs[device_count++], device, BUFSIZE);
            if (verbose)
                printf("Found mouse at: %s\n", device);
        }

        if (close(fd) == -1)
          panic("close failed on device: %s, error: %s", device, strerror(errno));

        if (device_count >= MAX_INPUTS) {
            if (verbose)
                printf("Warning: ran out of inputs while detecting devices\n");
            break;
        }
    }
}

void init_inputs() {
    int fd;
    int one = 1;

    for (int i = 0; i < device_count; i++) {
        if ((fd = open(named_inputs[i], O_RDONLY)) < 0)
            panic("Could not open: %s", named_inputs[i]);

        // set the device to nonblocking mode
        if (ioctl(fd, FIONBIO, &one) < 0)
            panic("Could set to nonblocking: %s", named_inputs[i]);

        // grab the input device
        if (ioctl(fd, EVIOCGRAB, &one) < 0)
            panic("Could not grab: %s", named_inputs[i]);

        input_fds[i] = fd;
    }
}

void init_outputs() {
    char *name;
    const char *suffix = " obfuskey";
    for (int i = 0; i < device_count; i++) {
        int err = libevdev_new_from_fd(input_fds[i], &output_devs[i]);

        if (err != 0)
            panic("Could not create evdev for input device: %s", named_inputs[i]);

        // Setting the device name under Qubes is pointless, as a Qubes VM
        // will never have a dynamically changing number of input devices like
        // a normal VM or a physical system. Furthermore, setting the device
        // name causes an alarming "Denied qubes.InputKeyboard from vm to
        // dom0" notification.
        if (!is_qubes_vm) {
            const char *tmp_name = libevdev_get_name(output_devs[i]);
            name = malloc(strlen(tmp_name) + strlen(suffix) + 1);
            if (name == NULL)
                panic("Could not allocate memory for device name: %s", tmp_name);

            strcpy(name, tmp_name);
            strcat(name, suffix);

            libevdev_set_name(output_devs[i], name);

            free(name);
        }

        err = libevdev_uinput_create_from_device(output_devs[i], LIBEVDEV_UINPUT_OPEN_MANAGED, &uidevs[i]);

        if (err != 0)
            panic("Could not create uidev for input device: %s", named_inputs[i]);
    }
}

void emit_event(struct entry *e) {
    int res, delay;
    long now = current_time_ms();
    delay = (int) (e->time - now);

    res = libevdev_uinput_write_event(uidevs[e->device_index], e->iev.type, e->iev.code, e->iev.value);
    if (res != 0) {
        panic("Failed to write event to uinput: %s", strerror(-res));
    }

    if (verbose) {
        printf("Released event at time : %ld. Device: %d,  Type: %*d,  "
               "Code: %*d,  Value: %*d,  Missed target:  %*d ms \n",
               e->time, e->device_index, 3, e->iev.type, 5, e->iev.code, 5, e->iev.value, 5, delay);
    }
}

void main_loop() {
    long int err;
    long prev_release_time = 0;
    long current_time = 0;
    long lower_bound = 0;
    long random_delay = 0;
    struct input_event ev;
    struct entry *n1, *np;

    // initialize the rescue state
    int rescue_state[MAX_RESCUE_KEYS];
    for (int i = 0; i < rescue_len; i++) {
        rescue_state[i] = 0;
    }

    // load input file descriptors for polling
    struct pollfd *pfds = calloc(device_count, sizeof(struct pollfd));
    if (pfds == NULL) {
        panic("Failed to allocate memory for pollfd array");
    }
    for (int j = 0; j < device_count; j++) {
        pfds[j].fd = input_fds[j];
        pfds[j].events = POLLIN;
    }

    // the main loop breaks when the rescue keys are detected
    // On each iteration, wait for input from the input devices
    // If the event is a key press/release, then schedule for
    // release in the future by generating a random delay. The
    // range of the delay depends on the previous event generated
    // so that events are always scheduled in the order they
    // arrive (FIFO).
    while (!interrupt) {
        // Emit any events exceeding the current time
        current_time = current_time_ms();
        while ((np = TAILQ_FIRST(&head)) && (current_time >= np->time)) {
            emit_event(np);
            TAILQ_REMOVE(&head, np, entries);
            free(np);
        }

        // Wait for next input event
        if ((err = poll(pfds, device_count, POLL_TIMEOUT_MS)) < 0)
            panic("poll() failed: %s\n", strerror(errno));

        // timed out, do nothing
        if (err == 0)
            continue;

        // An event is available, mark the current time
        current_time = current_time_ms();

        // Buffer the event with a random delay
        for (int k = 0; k < device_count; k++) {
            if (pfds[k].revents & POLLIN) {
                if ((err = read(pfds[k].fd, &ev, sizeof(ev))) <= 0)
                    panic("read() failed: %s", strerror(errno));

                // check for the rescue sequence.
                if (!persistent) {
                    if (ev.type == EV_KEY) {
                        int all = 1;
                        for (int j = 0; j < rescue_len; j++) {
                            if (rescue_keys[j] == ev.code)
                                rescue_state[j] = (ev.value == 0 ? 0 : 1);
                            all = all && rescue_state[j];
                        }
                        if (all)
                            interrupt = 1;
                    }
                }

                // schedule the keyboard event to be released sometime in the future.
                // lower bound must be bounded between time since last scheduled event and max delay
                // preserves event order and bounds the maximum delay
                lower_bound = min(max(prev_release_time - current_time, 0), max_delay);

                // syn events are not delayed
                if (ev.type == EV_SYN) {
                    random_delay = lower_bound;
                } else {
                    random_delay = random_between(lower_bound, max_delay);
                }

                // Buffer the event
                n1 = malloc(sizeof(struct entry));
                if (n1 == NULL) {
                    panic("Failed to allocate memory for entry");
                }
                n1->time = current_time + random_delay;
                n1->iev = ev;
                n1->device_index = k;
                TAILQ_INSERT_TAIL(&head, n1, entries);

                // Keep track of the previous scheduled release time
                prev_release_time = n1->time;

                if (verbose) {
                    printf("Buffered event at time: %ld. Device: %d,  Type: %*d,  "
                           "Code: %*d,  Value: %*d,  Scheduled delay: %*ld ms \n",
                           n1->time, k, 3, n1->iev.type, 5, n1->iev.code, 5, n1->iev.value,
                           4, random_delay);
                    if (lower_bound > 0) {
                        printf("Lower bound raised to: %*ld ms\n", 4, lower_bound);
                    }
                }
            }
        }
    }

    free(pfds);
}

void usage() {
    fprintf(stderr, "Usage: obfuskey [options]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -r filename: device file to read events from. Can specify multiple -r options.\n");
    fprintf(stderr, "  -d delay: maximum delay (milliseconds) of released events. Default 100.\n");
    fprintf(stderr, "  -s startup_timeout: time to wait (milliseconds) before startup. Default 500.\n");
    fprintf(stderr, "  -k csv_string: csv list of rescue key names to exit obfuskey in case the\n"
            "     keyboard becomes unresponsive. Default is 'KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_ESC'.\n");
    fprintf(stderr, "  -p: persistent mode (disable rescue key sequence)\n");
    fprintf(stderr, "  -v: verbose mode\n");
}

void banner() {
    printf("********************************************************************************\n"
           "* Started obfuskey : Keystroke-level Online Anonymizing Kernel\n"
           "* Maximum delay : %d ms\n"
           "* Reading from  : %s\n",
           max_delay, named_inputs[0]);

    for (int i = 1; i < device_count; i++) {
        printf("*                 %s\n", named_inputs[i]);
    }
    if (persistent) {
        printf("* Persistent mode, rescue keys disabled");
    } else {
        printf("* Rescue keys   : %s", lookup_keyname(rescue_keys[0]));
        for (int i = 1; i < rescue_len; i++) {
            printf(" + %s", lookup_keyname(rescue_keys[i]));
        }
    }

    printf("\n");
    printf("********************************************************************************\n");
}

int main(int argc, char **argv) {
    if (sodium_init() == -1) {
        panic("sodium_init failed");
    }

    if ((getuid()) != 0)
        printf("You are not root! This may not work...\n");

    if (access(qubes_detect_file, F_OK) == 0) {
        is_qubes_vm = true;
    }

    while (1) {
        int c = getopt_long(argc, argv, "r:d:s:k:vph", long_options, NULL);

        if (c < 0)
            break;

        switch (c) {
        case 'r':
            if (device_count >= MAX_INPUTS)
                panic("Too many -r options: can read from at most %d devices\n", MAX_INPUTS);
            strtcpy(named_inputs[device_count++], optarg, BUFSIZE);
            break;

        case 'd':
            if ((max_delay = atoi(optarg)) < 0)
                panic("Maximum delay must be >= 0\n");
            break;

        case 's':
            if ((startup_timeout = atoi(optarg)) < 0)
                panic("Startup timeout must be >= 0\n");
            break;

        case 'k':
            if (persistent) {
                panic("-k and -p options are mutually exclusive, try -h for help\n");
            }
            strtcpy(rescue_keys_str, optarg, BUFSIZE);
            custom_rescue = 1;
            break;

        case 'v':
            verbose = 1;
            break;

        case 'p':
            if (custom_rescue) {
                panic("-k and -p options are mutually exclusive, try -h for help\n");
            }
            persistent = 1;
            break;

        case 'h':
            usage();
            exit(0);
            break;

        default:
            usage();
            panic("Unknown option: %c \n", optopt);
        }
    }

    // autodetect devices if none were specified
    if (device_count == 0)
        detect_devices();

    // autodetect failed
    if (device_count == 0)
        panic("Unable to find any keyboards or mice. Specify which input device(s) to use with -r");

    // set rescue keys from the default sequence or -k arg
    set_rescue_keys(rescue_keys_str);

    // wait for pending events to finish, avoids keys being "held down"
    printf("Waiting %d ms...\n", startup_timeout);
    sleep_ms(startup_timeout);

    // open the input devices and create the output devices
    init_inputs();
    init_outputs();

    // initialize the event queue
    TAILQ_INIT(&head);

    banner();
    main_loop();

    // close everything
    cleanup();

    exit(EXIT_SUCCESS);
}
