#include <stdlib.h>                  // malloc...
#include <stdio.h>                   // perror, printf
#include <stdint.h>                  // UINT16_MAX
#include <stdbool.h>                 // bool
#include <errno.h>                   // EINVAL, ENOMEM, errno
#include <libgen.h>                  // basename
#include <string.h>                  // strcmp
#include <sys/types.h>               // gai_strerror
#include <sys/socket.h>              // gai_strerror
#include <netdb.h>                   // gai_strerror

#include "common.h"                  // ELEMENT_DUMP
#include "optparse.h"                // opt_*()
#include "pt_loop.h"                 // pt_loop_t
#include "probe.h"                   // probe_t
#include "lattice.h"                 // lattice_t
#include "algorithm.h"               // algorithm_instance_t
#include "algorithms/mda.h"          // mda_*_t
#include "algorithms/traceroute.h"   // traceroute_options_t
#include "address.h"                 // address_to_string
#include "options.h"

//---------------------------------------------------------------------------
// Command line stuff
//---------------------------------------------------------------------------

#define HELP_4        "Use IPv4."
#define HELP_6        "Use IPv6."
#define HELP_a        "Set the traceroute algorithm (default: 'paris-traceroute'). Valid values are 'paris-traceroute' and 'mda'."
#define HELP_d        "Set PORT as destination port (default: 33457)."
#define HELP_s        "Set PORT as source port (default: 33456)."
#define HELP_P        "Use raw packet of protocol PROTOCOL for tracerouting (default: 'udp')."
#define HELP_U        "Use UDP for tracerouting. The destination port is set by default to 53."
#define HELP_I        "Use ICMPv4/ICMPv6 for tracerouting."
#define HELP_v        "Print libparistraceroute debug information."
#define TEXT          "paris-traceroute - print the IP-level path toward a given IP host."
#define TEXT_OPTIONS  "Options:"

const char * algorithm_names[] = {
    "paris-traceroute", // default value
    "mda",
    NULL
};

static bool is_ipv4    = false;
static bool is_ipv6    = false;
static bool is_udp     = false;
static bool is_icmp    = false;
static bool is_verbose = false;

const char * protocol_names[] = {
    "udp",
    "icmp",
    NULL
};

// Bounded integer parameters
//                        def     min  max         option_enabled
static int dst_port[4] = {33457,  0,   UINT16_MAX, 0};
static int src_port[4] = {33456,  0,   UINT16_MAX, 1};

struct opt_spec runnable_options[] = {
    // action              sf          lf                   metavar             help          data
    {opt_text,             OPT_NO_SF,  OPT_NO_LF,           OPT_NO_METAVAR,     TEXT,         OPT_NO_DATA},
    {opt_text,             OPT_NO_SF,  OPT_NO_LF,           OPT_NO_METAVAR,     TEXT_OPTIONS, OPT_NO_DATA},
    {opt_store_1,          "4",        OPT_NO_LF,           OPT_NO_METAVAR,     HELP_4,       &is_ipv4},
    {opt_store_1,          "6",        OPT_NO_LF,           OPT_NO_METAVAR,     HELP_6,       &is_ipv6},
    {opt_store_1,          "v",        "--verbose",         OPT_NO_METAVAR,     HELP_v,       &is_verbose},
    {opt_store_choice,     "a",        "--algorithm",       "ALGORITHM",        HELP_a,       algorithm_names},
    {opt_store_int_lim_en, "s",        "--src-port",        "PORT",             HELP_s,       src_port},
    {opt_store_int_lim_en, "d",        "--dst-port",        "PORT",             HELP_d,       dst_port},
    {opt_store_choice,     "P",        "--protocol",        "PROTOCOL",         HELP_P,       protocol_names},
    {opt_store_1,          "U",        "--udp",             OPT_NO_METAVAR,     HELP_U,       &is_udp},
    {opt_store_1,          "I",        "--icmp",            OPT_NO_METAVAR,     HELP_I,       &is_icmp},
    END_OPT_SPECS
};

//---------------------------------------------------------------------------
// Main program
//---------------------------------------------------------------------------

/**
 * \brief Handle events raised by libparistraceroute
 * \param loop The main loop
 * \param event The event raised by libparistraceroute
 * \param user_data Points to user data, shared by
 *   all the algorithms instances running in this loop.
 */

void loop_handler(pt_loop_t * loop, event_t * event, void * user_data)
{
    traceroute_event_t         * traceroute_event;
    const traceroute_options_t * traceroute_options;
    const traceroute_data_t    * traceroute_data;
    mda_event_t                * mda_event;
    mda_data_t                 * mda_data;
    const char                 * algorithm_name;

    switch (event->type) {
        case ALGORITHM_TERMINATED:
            algorithm_name = event->issuer->algorithm->name;
            if (strcmp(algorithm_name, "mda") == 0) {
                mda_data = event->issuer->data;
                printf("Lattice:\n");
                lattice_dump(mda_data->lattice, (ELEMENT_DUMP) mda_lattice_elt_dump); 
                printf("\n");
            }
            pt_loop_terminate(loop);
            break;
        case ALGORITHM_EVENT:
            algorithm_name = event->issuer->algorithm->name;
            if (strcmp(algorithm_name, "mda") == 0) {
                mda_event = event->data;
                traceroute_options = event->issuer->options; // mda_options inherits traceroute_options
                switch (mda_event->type) {
                    case MDA_NEW_LINK:
                        mda_link_dump(mda_event->data, traceroute_options->do_resolv);
                        break;
                    default:
                        break;
                }
            } else if (strcmp(algorithm_name, "traceroute") == 0) {
                traceroute_event   = event->data;
                traceroute_options = event->issuer->options;
                traceroute_data    = event->issuer->data;

                // Forward this event to the default traceroute handler
                // See libparistraceroute/algorithms/traceroute.c
                traceroute_handler(loop, traceroute_event, traceroute_options, traceroute_data);
            }
            break;
        default:
            break;
    }
}

/**
 * \brief Prepare options supported by paris-traceroute
 * \return A pointer to the corresponding options_t instance if successfull, NULL otherwise
 */

static options_t * init_options(const char * version) {
    options_t * options;

    // Building the command line options
    if (!(options = options_create(NULL))) {
        goto ERR_OPTIONS_CREATE;
    }

    options_add_optspecs(options, runnable_options);
    options_add_optspecs(options, traceroute_get_options());
    options_add_optspecs(options, mda_get_options());
    options_add_optspecs(options, network_get_options());
    options_add_common  (options, version);
    return options;

ERR_OPTIONS_CREATE:
    return NULL;
}

int main(int argc, char ** argv)
{
    traceroute_options_t      traceroute_options;
    traceroute_options_t    * ptraceroute_options;
    mda_options_t             mda_options;
    const char              * version = "version 1.0";
    const char              * usage = "usage: %s [options] host\n";

    probe_t                 * probe;
    pt_loop_t               * loop;
    int                       exit_code = EXIT_FAILURE;
    int                       family;
    address_t                 dst_addr;
    options_t               * options;
    char                    * dst_ip;
    char                    * dst_ip_num;
    const char              * algorithm_name;
    const char              * ip_protocol_name;
    const char              * protocol_name;
    void                    * algorithm_options;

    //prepare the commande line options
    if (!(options = init_options(version))) {
        fprintf(stderr, "E: Can't initialize options\n");
        goto ERR_INIT_OPTIONS;
    }

    // Retrieve values passed in the command-line
    if (options_parse (options, usage, argv) != 1) {
        fprintf(stderr, "%s: destination required\n", basename(argv[0]));
        goto ERR_OPT_PARSE;
    }

    // We assume that the target IP address is always the last argument
    dst_ip = argv[argc - 1];

    // Retrieve the algorithm set by the user (or the default one)
    algorithm_name = algorithm_names[0];

    // Verify that the user pass option related to mda iif this is the chosen algorithm.
    if (options_mda_get_is_set()) {
        if (strcmp(algorithm_name, "mda") != 0) {
            perror("E: You cannot pass options related to mda when using another algorithm");
            goto ERR_INVALID_ALGORITHM;
        }
    }

    // If no one is set, call address_guess_family.
    // If only one is set to true, set family to AF_INET or AF_INET6
    // If the both have been set, print an error.
    if (is_ipv4 && !is_ipv6) {
        family = AF_INET;
    } else if (is_ipv6 && !is_ipv4) {
        family = AF_INET6;
    } else if (is_ipv4 && is_ipv6) {
        fprintf(stderr, "Can not set both ip versions\n");
        goto ERR_IP_VERSIONS_CONFLICT;
    } else {
        // Get address family if not defined by the user
        if (!address_guess_family(dst_ip, &family)) goto ERR_ADDRESS_GUESS_FAMILY;
    }

    switch (family) {
        case AF_INET:
            ip_protocol_name = "ipv4";
            break;
        case AF_INET6:
            ip_protocol_name = "ipv6";
            break;
        default:
            fprintf(stderr, "Internet family not supported (%d)\n", family);
            goto ERR_FAMILY;
    }

    // Translate the string IP / FQDN into an address_t * instance
    if (address_from_string(family, dst_ip, &dst_addr) != 0) {
        fprintf(stderr, "E: Invalid destination address %s\n", dst_ip);
        goto ERR_ADDRESS_IP_FROM_STRING;
    }

    // If dst_ip is not a FQDN, we've to get the corresponding IP string.
    // This is a crappy patch while ipv*.c protocols module do not use field carrying address_t instance
    if ((address_to_string(&dst_addr, &dst_ip_num)) != 0) goto ERR_ADDRESS_TO_STRING;

    // Prepare data

    // Probe skeleton definition: IPv4/UDP probe targetting 'dst_ip'
    if (!(probe = probe_create())) {
        perror("E: Cannot create probe skeleton");
        goto ERR_PROBE_SKEL;
    }

    is_icmp |= (strcmp(protocol_names[0], "icmp") == 0);
    is_udp  |= (strcmp(protocol_names[0], "udp")  == 0);

    // TODO akram control whether is_icmp && is_udp are not both set to true

    if (is_icmp) {
        switch (family) {
            case AF_INET:
                protocol_name = "icmpv4";
                break;
            case AF_INET6:
                protocol_name = "icmpv6";
                break;
            default:
                fprintf(stderr, "Internet family not supported (%d)\n", family);
                goto ERR_FAMILY2;
        }
    }

    // Prepare probe 
    probe_set_protocols(probe, ip_protocol_name, protocol_name, NULL);
    probe_set_field(probe, ADDRESS("dst_ip", &dst_addr));

    if (!is_icmp) {
        probe_set_fields(
            probe,
            I16    ("dst_port", dst_port[0]),
            I16    ("src_port", src_port[0]),
            NULL
        );
        probe_payload_resize(probe, 2);
    }

    // Option -U sets port to 53 (DNS) and dst_port not explicitely set
    if (is_udp && !dst_port[3]) {
        probe_set_fields(probe, I16("dst_port", 53), NULL);
    }

    // Dedicated options (if any)
    if (strcmp(algorithm_name, "paris-traceroute") == 0) {
        traceroute_options  = traceroute_get_default_options();
        ptraceroute_options = &traceroute_options;
        algorithm_options   = &traceroute_options;
        algorithm_name      = "traceroute";
    } else if ((strcmp(algorithm_name, "mda") == 0) || options_mda_get_is_set()) {
        mda_options            = mda_get_default_options();
        ptraceroute_options    = &mda_options.traceroute_options;
        algorithm_options      = &mda_options;
        mda_options.bound      = options_mda_get_bound();
        mda_options.max_branch = options_mda_get_max_branch();
    } else {
        perror("E: Unknown algorithm");
        goto ERR_UNKNOWN_ALGORITHM;
    }

    // Common options
    if (ptraceroute_options) {
        // TODO akram: add options_traceroute_init() in algorithms/traceroute.c which
        // initializes a traceroute_options_t * structure by using command line options.
        // Then, remove the following options. Same for job for mda.
        ptraceroute_options->min_ttl          = options_traceroute_get_min_ttl();
        ptraceroute_options->max_ttl          = options_traceroute_get_max_ttl();
        ptraceroute_options->num_probes       = options_traceroute_get_num_queries();
        ptraceroute_options->max_undiscovered = options_traceroute_get_max_undiscovered();
        ptraceroute_options->dst_addr         = &dst_addr;
        ptraceroute_options->do_resolv        = options_traceroute_get_do_resolv();
    }

    // Create libparistraceroute loop
    if (!(loop = pt_loop_create(loop_handler, NULL))) {
        perror("E: Cannot create libparistraceroute loop");
        goto ERR_LOOP_CREATE;
    }

    // Set network timeout
    network_set_timeout(loop->network, options_network_get_timeout());
    network_set_is_verbose(loop->network, is_verbose);

    printf("Traceroute to %s (%s), %u hops max, %ld bytes packets\n\n",
        dst_ip,
        dst_ip_num,
        ptraceroute_options->max_ttl,
        packet_get_size(probe->packet)
    );

    // Add an algorithm instance in the main loop
    if (!pt_algorithm_add(loop, algorithm_name, algorithm_options, probe)) {
        perror("E: Cannot add the chosen algorithm");
        goto ERR_INSTANCE;
    }

    // Wait for events. They will be catched by handler_user()
    if (pt_loop(loop, 0) < 0) {
        perror("E: Main loop interrupted");
        goto ERR_PT_LOOP;
    }
    exit_code = EXIT_SUCCESS;

    // Leave the program
ERR_PT_LOOP:
ERR_INSTANCE:
ERR_UNKNOWN_ALGORITHM:
    probe_free(probe);
ERR_FAMILY2:
ERR_PROBE_SKEL:
    // pt_loop_free() automatically removes algorithms instances,
    // probe_replies and events from the memory.
    // Options and probe must be manually removed.
    pt_loop_free(loop);
ERR_LOOP_CREATE:
    if (dst_ip_num) free(dst_ip_num); // allocated by address_to_string
ERR_ADDRESS_TO_STRING:
ERR_ADDRESS_IP_FROM_STRING:
ERR_FAMILY:
ERR_ADDRESS_GUESS_FAMILY:
ERR_IP_VERSIONS_CONFLICT:
ERR_INVALID_ALGORITHM:
    if (errno) perror(gai_strerror(errno));
ERR_OPT_PARSE:
ERR_INIT_OPTIONS:
    exit(exit_code);
}

