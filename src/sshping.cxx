/*
   Copyright (c) 2017 by Uncle Spook

   MIT License

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
 */

#include <algorithm>
#include <inttypes.h>
#include <iostream>
#include <libssh/libssh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#include "optionparser.h"

struct timespec t0;
struct timespec t1;
int             verbosity = 0;
int             num_chars = 1000;
char*           port      = NULL;
char*           addr      = NULL;
char*           user      = NULL;
std::string     echo_cmd  = "cat > /dev/null";

/* *INDENT-OFF* */
// Define a required argument for optionparse
struct Arg
    : public option::Arg {
    static option::ArgStatus Reqd(const option::Option & option, bool msg) {
        if (option.arg != 0) return option::ARG_OK;
        if (msg) fprintf(stderr, "Option '%s' requires an argument\n", option.name);
        return option::ARG_ILLEGAL;
    }
};

// CLI options and usage help
enum  { opNONE,
        opNUM,
        opECMD,
        opHELP,
        opID,
        opPWD,
        opTIME,
        opTEST,
        opVERB };
const option::Descriptor usage[] = {
    {opNONE, 0, "",  "",          Arg::None, "Usage: sshping [options] [user@]addr[:port]" },
    {opNONE, 0, "",  "",          Arg::None, " " },
    {opNONE, 0, "",  "",          Arg::None, "  SSH-based ping that measures interactive character echo latency." },
    {opNONE, 0, "",  "",          Arg::None, "  Pronounced \"shipping\"." },
    {opNONE, 0, "",  "",          Arg::None, " " },
    {opNONE, 0, "",  "",          Arg::None, "Options:" },
    {opNUM,  0, "c", "count",     Arg::Reqd, "  -c  --count NCHARS   Number of characters to echo, default 1000"},
    {opECMD, 0, "e", "echocmd",   Arg::Reqd, "  -e  --echocmd CMD    Use CMD for echo command; default: cat > /dev/null"},
    {opHELP, 0, "h", "help",      Arg::None, "  -h  --help           Print usage and exit"},
    {opID,   0, "i", "identity",  Arg::Reqd, "  -i  --identity FILE  Identity file, ie ssh private keyfile"},
    {opPWD,  0, "p", "password",  Arg::Reqd, "  -p  --password PWD   Use password PWD (can be seen, use with care)"},
    {opTIME, 0, "r", "runtime",   Arg::Reqd, "  -r  --runtime SECS   Run for SECS seconds, instead of count limit"},
    {opTEST, 0, "t", "tests",     Arg::Reqd, "  -t  --tests e|s      Run tests e=echo s=speed; default es=both"},
    {opVERB, 0, "v", "verbose",   Arg::None, "  -v  --verbose        Show more output, use twice for more: -vv"},
    {0,0,0,0,0,0}
};
/* *INDENT-ON* */

// Outta here!
void die(const char* msg) {
    fprintf(stderr, "*** %s\n", msg);
    exit(255);
}

// Nanosecond difference between two timestamps
uint64_t nsec_diff(const struct timespec & t0,
                   const struct timespec & t1) {
    uint64_t u0 = t0.tv_sec * 1000000000 + t0.tv_nsec;
    uint64_t u1 = t1.tv_sec * 1000000000 + t1.tv_nsec;
    return u1 > u0 ? u1 - u0 : u0 - u1;
}

// nanoseconds to milliseconds
int to_msec(uint64_t nsecs) {
    return nsecs / 1000000;
}

// Consume all pending output and discard it
int discard_output(ssh_channel & chn,
                   int           max_wait = 1000) {
    char buffer[256];
    while (ssh_channel_is_open(chn) && !ssh_channel_is_eof(chn)) {
        int nbytes = ssh_channel_read_timeout(chn,
                                              buffer,
                                              sizeof(buffer),
                                              /*is-stderr*/ 0,
                                              max_wait);
        if (nbytes < 0) {
            return SSH_ERROR;
        }
        if (nbytes == 0) {
            return SSH_OK; // timeout, we're done
        }
    }
    return SSH_ERROR;
}

// Start the session to the target system
ssh_session begin_session() {

    // Create session
    ssh_session ses;
    ses = ssh_new();
    if (ses == NULL) {
        return NULL;
    }

    // Set options
    int nport      = atoi(port);
    int sshverbose = verbosity >= 2 ? SSH_LOG_PROTOCOL : 0;
    ssh_options_set(ses, SSH_OPTIONS_HOST, addr);
    ssh_options_set(ses, SSH_OPTIONS_PORT, &nport);
    if (!user) {
        ssh_options_set(ses, SSH_OPTIONS_USER, user);
    }
    ssh_options_set(ses, SSH_OPTIONS_LOG_VERBOSITY, &sshverbose);

    // Try to connect
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = ssh_connect(ses);
    if (rc != SSH_OK) {
        fprintf(stderr, "*** Error connecting: %s\n", ssh_get_error(ses));
        return NULL;
    }

    rc = ssh_userauth_publickey_auto(ses, NULL, NULL);
    if (rc == SSH_AUTH_ERROR) {
        fprintf(stderr, "*** Authentication failed: %s\n", ssh_get_error(ses));
        return NULL;
    }
    /*
       rc = ssh_userauth_password(ses, NULL, argv[0]);
       if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "Error authenticating with password: %s\n",
                ssh_get_error(ses));
        ssh_disconnect(ses);
        ssh_free(ses);
        exit(-1);
       }
     */
    if (verbosity) {
        printf("+++ Connected to %s:%s\n", addr, port);
    }
    return ses;
}

// Login to a shell
ssh_channel login_channel(ssh_session & ses) {
    // Start the channel
    ssh_channel chn = ssh_channel_new(ses);
    if (chn == NULL) {
        return NULL;
    }
    int rc = ssh_channel_open_session(chn);
    if (rc != SSH_OK) {
        ssh_channel_free(chn);
        return NULL;
    }

    // Make it be interactive-like
    rc = ssh_channel_request_pty(chn);
    if (rc != SSH_OK) {
        ssh_channel_free(chn);
        return NULL;
    }
    rc = ssh_channel_change_pty_size(chn, 80, 24);
    if (rc != SSH_OK) {
        ssh_channel_free(chn);
        return NULL;
    }

    // Run a shell
    rc = ssh_channel_request_shell(chn);
    if (rc != SSH_OK) {
        ssh_channel_free(chn);
        return NULL;
    }

    // Flush output from the login
    rc = discard_output(chn, 1300);
    if (rc != SSH_OK) {
        ssh_channel_free(chn);
        return NULL;
    }

    // --- Marker: Timing point for the initial handshake
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (verbosity) {
        printf("+++ Login shell established\n");
    }
    printf("--- Login: %d msec\n", to_msec(nsec_diff(t0, t1)));

    return chn;
}

// Run a single-character-at-a-time echo test
int run_echo_test(ssh_channel & chn) {

    // Start the echo server
    echo_cmd += "\n";
    int nbytes = ssh_channel_write(chn, echo_cmd.c_str(), echo_cmd.length());
    if (nbytes != echo_cmd.length()) {
        return SSH_ERROR;
    }
    int rc = discard_output(chn, 1500);
    if (rc != SSH_OK) {
        return rc;
    }
    if (verbosity) {
        printf("+++ Echo responder started\n");
    }

    //  Send one character at a time, read back the response, getting timing data as we go
    uint64_t tot_latency = 0;
    uint64_t min_latency = 0;
    uint64_t max_latency = 0;

    char wbuf[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n";
    char rbuf[2];
    std::vector<uint64_t> latencies;
    for (int n = 0; n < num_chars; n++) {

        // Timing: begin
        struct timespec tw;
        clock_gettime(CLOCK_MONOTONIC, &tw);

        int i = n % (sizeof(wbuf) - 1);
        nbytes = ssh_channel_write(chn, &wbuf[i], 1);
        if (nbytes != 1) {
            fprintf(stderr, "\n*** write put %d bytes, expected 1\n", nbytes);
            return SSH_ERROR;
        }
        nbytes = ssh_channel_read_timeout(chn, &rbuf, 1, /*is-stderr*/ 0, 2500);
        if (nbytes != 1) {
            fprintf(stderr, "\n*** read got %d bytes, expected 1\n", nbytes);
            return SSH_ERROR;
        }
        // Timing: end
        struct timespec tr;
        clock_gettime(CLOCK_MONOTONIC, &tr);
        uint64_t latency = nsec_diff(tw, tr);
        latencies.push_back(latency);
        tot_latency += latency;
        if (!min_latency || (latency < min_latency)) min_latency = latency;
        if (latency > max_latency) max_latency = latency;
    }

    uint64_t avg_latency = tot_latency / num_chars;
    uint64_t med_latency;
    std::sort(latencies.begin(), latencies.end());
    size_t nlat = latencies.size();
    if (nlat & 1) {
        med_latency = latencies[(nlat+1)/2 - 1];
    }
    else {
        med_latency = (latencies[nlat/2 - 1] + latencies[(nlat+1)/2 - 1]) / 2;
    }
    uint64_t stddev = 0;
    printf("--- Minimum Latency: %" PRIu64 " nsec\n", min_latency);
    printf("---  Median Latency: %" PRIu64 " nsec  +/- %" PRIu64" std dev\n", med_latency, stddev);
    printf("--- Average Latency: %" PRIu64 " nsec\n", avg_latency);
    printf("--- Maximum Latency: %" PRIu64 " nsec\n", max_latency);

    // Terminate the echo responder
        // TODO
    if (verbosity) {
        printf("+++ Echo responder finished\n");
    }
    return SSH_OK;
}

// Run a speed test
int run_speed_test(ssh_session ses) {

    if (verbosity) {
        printf("+++ Speed test started\n");
    }

    ssh_scp scp = ssh_scp_new(ses, SSH_SCP_WRITE, "/dev/null");
    if (scp == NULL) {
        fprintf(stderr, "*** Cannot allocate scp context: %s\n", ssh_get_error(ses));
        return SSH_ERROR;
    }
    int rc = ssh_scp_init(scp);
    if (rc != SSH_OK) {
        fprintf(stderr, "*** Cannot init scp context: %s\n", ssh_get_error(ses));
        ssh_scp_free(scp);
        return rc;
    }

    #define BUFLEN 8000000
    char buf[BUFLEN];
    memset(buf, 's', BUFLEN);
    rc = ssh_scp_push_file(scp, "speedtest.tmp", BUFLEN, S_IRUSR);
    if (rc != SSH_OK) {
        fprintf(stderr, "*** Can't open remote file: %s\n", ssh_get_error(ses));
        return rc;
    }

    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    rc = ssh_scp_write(scp, buf, BUFLEN);
    if (rc != SSH_OK) {
        fprintf(stderr, "*** Can't write to remote file: %s\n", ssh_get_error(ses));
        return rc;
    }
    struct timespec t3;
    clock_gettime(CLOCK_MONOTONIC, &t3);

    ssh_scp_close(scp);
    ssh_scp_free(scp);

    double duration = double(nsec_diff(t3, t2))/1000000000.0;
    if (duration == 0.0) duration = 0.1;
    uint64_t Bps = double(BUFLEN)/duration;

    printf("---  Transfer Speed: %" PRIu64 " Bytes/second\n", Bps);
    if (verbosity) {
        printf("+++ Speed test completed\n");
    }
    return SSH_OK;
}

void logout_channel(ssh_channel & chn) {
    // All done, cleanup
    ssh_channel_close(chn);
    ssh_channel_send_eof(chn);
    ssh_channel_free(chn);
    if (verbosity) {
        printf("+++ Login shell closed\n");
    }
}

void end_session(ssh_session & ses) {
    ssh_disconnect(ses);
    ssh_free(ses);
    if (verbosity) {
        printf("+++ Disconnected\n");
    }

}


int main(int   argc,
         char* argv[]) {

    // Process the command line
    argc -= (argc > 0);argv += (argc > 0); // skip program name argv[0] if present
    option::Stats  stats(usage, argc, argv);
    option::Option opts[stats.options_max], buffer[stats.buffer_max];
    option::Parser parse(usage, argc, argv, opts, buffer);
    if (opts[opHELP]) {
        option::printUsage(std::cerr, usage);
        return 0;
    }
    if (parse.error() || (argc < 1) || (parse.nonOptionsCount() != 1)) {
        option::printUsage(std::cerr, usage); // I wish it didn't use streams
        fprintf(stderr, "\n*** Command error, see usage\n");
        return 255;
    }
    bool anyunk = false;
    for (option::Option* opt = opts[opNONE]; opt; opt = opt->next()) {
        if (!anyunk) {
            option::printUsage(std::cerr, usage);
        }
        fprintf(stderr, "*** Unknown option %s\n", opt->name);
        anyunk = true;
    }
    if (anyunk) {
        return 255;
    }

    port = (char*)parse.nonOption(0);
    addr = strsep(&port, ":");
    user = strsep(&addr, "@");
    if (!addr || !addr[0]) {
        addr = user;
        user = NULL;
    }
    if (!port || !port[0]) {
        port = (char*)"22";
    }
    int nport = atoi(port);
    if (!nport || (nport < 1) || (nport > 65535)) {
        fprintf(stderr, "*** Bad port, must be integer from 1 to 65535\n");
        exit(255);
    }

    if (opts[opECMD]) {
        echo_cmd = opts[opECMD].arg;
    }
    if (opts[opNUM]) {
        num_chars = atoi(opts[opNUM].arg);
    }
    verbosity = opts[opVERB].count();

    if (verbosity) {
        printf("User: %s\n", user ? user : "--not specified--");
        printf("Host: %s\n", addr);
        printf("Port: %d\n", nport);
        printf("Echo: %s\n", echo_cmd.c_str());
        printf("\n");
    }

    // Begin Session and login
    ssh_session ses = begin_session();
    if (!ses) {
        die("Cannot establish ssh session");
    }
    ssh_channel chn = login_channel(ses);
    if (!chn) {
        die("Cannot login and run echo command");
    }

    // Run the tests
    bool do_echo  = !opts[opTEST] || strchr(opts[opTEST].arg, 'e');
    bool do_speed = !opts[opTEST] || strchr(opts[opTEST].arg, 's');
    if (do_echo)  run_echo_test(chn);
    if (do_speed) run_speed_test(ses);

    // Cleanup
    logout_channel(chn);
    end_session(ses);
}