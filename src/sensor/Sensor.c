/*
 * Copyright (C) 2015 by Damian Pfammatter <pfammatterdamian@gmail.com>
 *
 * This file is part of RTL-spec.
 *
 * RTL-Spec is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * RTL-Spec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RTL-Spec.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <assert.h>
#include <zlib.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>

#include "../include/UTI.h"
#include "../include/THR.h"
#include "../include/SDR.h"
#include "../include/ITE.h"
#include "../include/QUE.h"
#include "../include/FFT.h"
#include "../include/uthash.h"

#define DEFAULT_LOG2_FFT_SIZE 8
#define DEFAULT_MONITOR_TIME 0
#define DEFAULT_MIN_TIME_RES 0
#define DEFAULT_DEV_INDEX 0
#define DEFAULT_CLK_OFF 0
#define DEFAULT_CLK_CORR_PERIOD 3600
#define DEFAULT_HOPPING_STRATEGY_STR "similarity"
#define DEFAULT_GAIN 32.8f
#define DEFAULT_FREQ_OVERLAP (1/6.f)
#define DEFAULT_AVG_FACTOR 5
#define DEFAULT_SOVERLAP (1<<DEFAULT_LOG2_FFT_SIZE)/2
#define DEFAULT_WINDOW_FUN_STR "hanning"
#define DEFAULT_FFT_BATCHLEN 10
#define DEFAULT_SAMP_RATE 2400000

#define SEQUENTIAL_HOPPING_STRATEGY     0
#define RANDOM_HOPPING_STRATEGY         1
#define SIMILARITY_HOPPING_STRATEGY     2

#define RECTANGULAR_WINDOW              0
#define HANNING_WINDOW                  1
#define BLACKMAN_HARRIS_WINDOW          2

#define THR_MANAGER    0
#define THR_FREQ_CORR  1
#define THR_SPEC_MONI  2
#define THR_SAMP_WIND  3
#define THR_FFT        4
#define THR_AVG        5
#define THR_DUMP       8

#define FLAG_FREQ_CORR 1
#define FLAG_SPEC_MONI 2
#define FLAG_SAMP_WIND 4

#define STR_LEN 30
#define STR_LEN_LONG 512

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define TICK(tstart) clock_gettime(CLOCK_MONOTONIC, &tstart)
#define TACK(tstart, tend, file) clock_gettime(CLOCK_MONOTONIC, &tend);fprintf(file, "%.3f\n", ((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) - ((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec))

typedef float (*window_fun_ptr_t)(int, int);

typedef struct {
	Thread       *thread;
	unsigned int min_freq;
	unsigned int max_freq;
	unsigned int clk_corr_period;
	unsigned int samp_rate;
	unsigned int log2_fft_size;
	unsigned int avg_factor;
	unsigned int soverlap;
	unsigned int monitor_time;
	unsigned int min_time_res;
	unsigned int number_of_sample_runs;
	unsigned int fft_batchlen;
	int          dev_index;
	int          clk_off;
	float        gain;
	float        freq_overlap;
	char         *hopping_strategy_str;
	char         *window_fun_str;
} ManagerCTX;

typedef struct {
	Thread       *thread;
	int          dev_index;
	int          clk_off;
} FrequencyCorrectionCTX;

typedef struct {
	ManagerCTX             *manager_ctx;
	FrequencyCorrectionCTX *freq_corr_ctx;
} FrequencyCorrectionARG;

typedef struct {
	Thread       *thread;
	unsigned int min_freq;
	unsigned int max_freq;
	unsigned int samp_rate;
	unsigned int log2_fft_size;
	unsigned int avg_factor;
	unsigned int soverlap;
	unsigned int monitor_time;
	unsigned int min_time_res;
	unsigned int number_of_sample_runs;
	unsigned int fft_batchlen;
	int          hopping_strategy_id;
	int          window_fun_id;
	int          dev_index;
	int          clk_off;
	float        gain;
	float        freq_overlap;
} SpectrumMonitoringCTX;

typedef struct {
	ManagerCTX            *manager_ctx;
	SpectrumMonitoringCTX *spec_moni_ctx;
} SpectrumMonitoringARG;

typedef struct {
	Thread           *thread;
	void             (*callback)(Item *);
	unsigned int     length;
	unsigned int     *samp_rates;
	unsigned int     *log2_fft_sizes;
	unsigned int     *avg_factors;
	unsigned int     *soverlaps;
	unsigned int     *center_freqs;
	int              hopping_strategy_id;
	int              window_fun_id;
	int              clk_off;
	float            gain;
	float            *freq_overlaps;
	window_fun_ptr_t *window_funs;
} SamplingWindowingCTX;

typedef struct {
	SpectrumMonitoringCTX *spec_moni_ctx;
	SamplingWindowingCTX  *samp_wind_ctx;
	Queue                 *qin, **qsout;
	size_t                qsout_cnt;
} SamplingWindowingARG;

typedef struct {
	Thread       *thread;
	void         (*callback)(Item *);
	unsigned int fft_batchlen;
} FFTCTX;

typedef struct {
	SpectrumMonitoringCTX *spec_moni_ctx;
	FFTCTX                *fft_ctx;
	Queue                 *qin, **qsout;
	size_t                qsout_cnt;
} FFTARG;

typedef struct {
	Thread *thread;
	void (*callback)(Item *);
} AveragingCTX;

typedef struct {
	SpectrumMonitoringCTX *spec_moni_ctx;
	AveragingCTX          *avg_ctx;
	Queue                 *qin, **qsout;
	size_t                qsout_cnt;
} AveragingARG;


typedef struct {
	Thread       *thread;
} DumpingCTX;

typedef struct {
	DumpingCTX   *dump_ctx;
	Queue        *qin, **qsout;
	int          qsout_cnt;
} DumpingARG;

/*! RTL-SDR Device Handle
 * 
 * NOTE: Make sure to acquire the lock 'rtlsdr_mut' before accessing 'rtlsdr_dev'. This guarantees
 * mutual exclusive usage of the RTL-SDR device. Release the lock when finished.
 */
static rtlsdr_dev_t *rtlsdr_dev;
static pthread_mutex_t *rtlsdr_mut;


static void* frequency_correction(void *args);
static void* spectrum_monitoring(void *args);

/*! Signal Processing - Sampling and Windowing
 * 
 * Read I/Q sample stream from RTL-SDR device and multiply with windowing function.
 * 
 * \param args
 */
static void* sampling_windowing(void *args);

/*! Signal Processing - Fast Fourier Transform and Envelope Detection
 * 
 * The FFT is calculated on the GPU. To mitigate communication overhead between CPU and GPU, FFT
 * jobs are transfered in batches. Within a batch, all jobs must have the same FFT size. This is
 * because the FFT resources on the GPU have to be reinitialized for different FFT sizes. Therefore,
 * frequently changing FFT sizes can impact FFT performance. We could optimize this by having
 * different batches for different FFT sizes. However, this would lead to the problem that jobs of
 * low frequent FFT sizes get delayed in the signal processing chain.
 * 
 * The output samples are in dB, i.e. envelope detection is performed as part of this signal
 * processing block.
 */
static void* fft(void *args);

/*! Signal Processing - Averaging
 */
static void* averaging(void *args);

/*! Signal Processing - Dump data to stdout
 */
static void* dumping(void *args);

int main(int argc, char *argv[]) {

	ManagerCTX *manager_ctx;
	FrequencyCorrectionCTX *freq_corr_ctx;
	SpectrumMonitoringCTX *spec_moni_ctx;

	FrequencyCorrectionARG *freq_corr_arg;
	SpectrumMonitoringARG *spec_moni_arg;

	// Ctrl-C signal catcher
	void terminate(int sig) {

		if(sig == SIGINT) fprintf(stderr, "\nCtrl-C caught. Waiting for termination...\n");

		// Try clean termination for 120 seconds, afterwards abruptly abort
		void time_out(int sig) {
			fprintf(stderr, "Aborted.\n");
			exit(1);
		}
		signal(SIGALRM, time_out);
		alarm(60);

		pthread_mutex_unlock(manager_ctx->thread->lock);

		// Ask sensor threads to terminate
		pthread_mutex_lock(freq_corr_ctx->thread->lock);
		freq_corr_ctx->thread->is_running = 0;
		pthread_mutex_unlock(freq_corr_ctx->thread->lock);

		pthread_mutex_lock(spec_moni_ctx->thread->lock);
		spec_moni_ctx->thread->is_running = 0;
		pthread_mutex_unlock(spec_moni_ctx->thread->lock);

		// Awake sensor threads when sleeping
		pthread_cond_signal(freq_corr_ctx->thread->awake);
		pthread_cond_signal(spec_moni_ctx->thread->awake);

		// Join sensor threads
		pthread_join(*(freq_corr_ctx->thread->fd), NULL);
		pthread_join(*(spec_moni_ctx->thread->fd), NULL);

		// Free sensor arguments
		free(spec_moni_arg);
		free(freq_corr_arg);

		// Free RTL-SDR device and associated lock
		SDR_release(rtlsdr_dev);
		pthread_mutex_destroy(rtlsdr_mut);
		free(rtlsdr_mut);

		// Free sensor threads
		THR_release(spec_moni_ctx->thread);
		THR_release(freq_corr_ctx->thread);
		THR_release(manager_ctx->thread);
		//FIXME free dump thread?

		// Free sensor contexts
		free(spec_moni_ctx);
		free(freq_corr_ctx);
		free(manager_ctx);
		//FIXME free dump ctx?

#if defined(VERBOSE)
		fprintf(stderr, "[SMAN] Terminated.\n");
#endif

		fprintf(stderr, "Terminated.\n");
		pthread_exit(NULL);
	}

	// Parse arguments and options
	void parse_args(int argc, char *argv[]) {
		int opt;
		const char *options = "hd:c:k:g:y:s:f:b:a:o:q:t:r:w:x:";

		// Option arguments
		while((opt = getopt(argc, argv, options)) != -1) {
			switch(opt) {
			case 'h':
				goto usage;
			case 'd':
				manager_ctx->dev_index = atoi(optarg);
				break;
			case 'c':
				manager_ctx->clk_off = atoi(optarg);
				break;
			case 'k':
				manager_ctx->clk_corr_period = atol(optarg);
				break;
			case 'g':
				manager_ctx->gain = atof(optarg);
				break;
			case 'y':
				manager_ctx->hopping_strategy_str = optarg;
				break;
			case 's':
				manager_ctx->samp_rate = atol(optarg);
				break;
			case 'f':
				manager_ctx->log2_fft_size = atol(optarg);
				break;
			case 'b':
				manager_ctx->fft_batchlen = atol(optarg);
				break;
			case 'a':
				manager_ctx->avg_factor = atol(optarg);
				if(manager_ctx->avg_factor < 1) manager_ctx->avg_factor = DEFAULT_AVG_FACTOR;
				break;
			case 'o':
				manager_ctx->soverlap = atol(optarg);
				if(manager_ctx->soverlap > (1<<manager_ctx->log2_fft_size)-1)
					manager_ctx->soverlap = (1<<manager_ctx->log2_fft_size)/2;
				break;
			case 'q':
				manager_ctx->freq_overlap = atof(optarg);
				break;
			case 't':
				manager_ctx->monitor_time = atol(optarg);
				break;
			case 'r':
				manager_ctx->min_time_res = atol(optarg);
				break;
			case 'w':
				manager_ctx->window_fun_str = optarg;
				break;
			case 'x':
				manager_ctx->number_of_sample_runs = atol(optarg);
				break;
			default:
				goto usage;
			}
		}

		// Non-option arguments
		if(optind+2 != argc) {
			usage:
			fprintf(stderr,
					"Usage:\n"
					"  %s min_freq max_freq\n"
					"  [-h]\n"
					"  [-d <dev_index>]\n"
					"  [-c <clk_off>] [-k <clk_corr_period>]\n"
					"  [-g <gain>]\n"
					"  [-y <hopping_strategy>]\n"
					"  [-s <samp_rate>]\n"
					"  [-f <log2_fft_size>] [-b <fft_batchlen>]\n"
					"  [-a <avg_factor>] [-o <soverlap>] [-q <freq_overlap>]\n"
					"  [-t <monitor_time>] [-r <min_time_res>]\n"
					"  [-w <window>]\n"
					"  [-x <sample runs>]\n"
					"\n"
					"Arguments:\n"
					"  min_freq               Lower frequency bound in Hz\n"
					"  max_freq               Upper frequency bound in Hz\n"
					"\n"
					"Options:\n"
					"  -h                     Show this help\n"
					"  -d <dev_index>         RTL-SDR device index [default=%i]\n"
					"  -c <clk_off>           Clock offset in PPM [default=%i]\n"
					"  -k <clk_corr_period>   Clock correction period in seconds [default=%u]\n"
					"                           i.e. perform frequency correction every 'clk_corr_period'\n"
					"                           seconds\n"
					"  -g <gain>              Gain value in dB [default=%.1f]\n"
					"                           -1 for automatic gain\n"
					"  -y <hopping_strategy>  Hopping strategy to use [default=%s]\n"
					"                           sequential\n"
					"                           random\n"
					"                           similarity\n"
					"  -s <samp_rate>         Sampling rate in Hz [default=%u]\n"
					"  -f <log2_fft_size>     Use FFT size of 2^'log2_fft_size' [default=%u]\n"
					"                           the resulting frequency resolution is\n"
					"                           'samp_rate'/(2^'log2_fft_size')\n"
					"  -b <fft_batchlen>      FFT batch length [default=%u]\n"
					"                           i.e. process FFTs in batches of length 'fft_batchlen'\n"
					"  -a <avg_factor>        Averaging factor [default=%u]\n"
					"                           i.e. average 'avg_factor' segments\n"
					"  -o <soverlap>          Segment overlap [default=%u]\n"
					"                           i.e. number of samples per segment that overlap\n"
					"                           The time to dwell in seconds at a given frequency is given by\n"
					"                           (((1<<'log2_fft_size')-'soverlap')*'avg_factor'+'soverlap')/'samp_rate'\n"
					"  -q <freq_overlap>      Frequency overlapping factor [default=%.3f]\n"
					"                           i.e. the frequency width is reduced from 'samp_rate' to\n"
					"                           (1-'freq_overlap')*'samp_rate'\n"
					"  -t <monitor_time>      Time in seconds to monitor [default=%u]\n"
					"                           0 to monitor infinitely\n"
					"  -r <min_time_res>      Minimal time resolution in seconds [default=%u]\n"
					"                           0 for no time resolution limitation\n"
					"  -w <window>            Windowing function [default=%s]\n"
					"                           rectangular\n"
					"                           hanning\n"
					"                           blackman_harris_4\n"
					"  -x <sample runs>       Stops after N times sampling the band. 0 means off (default)\n"
					"",
					argv[0],
					manager_ctx->dev_index,
					manager_ctx->clk_off, manager_ctx->clk_corr_period,
					manager_ctx->gain,
					manager_ctx->hopping_strategy_str,
					manager_ctx->samp_rate,
					manager_ctx->log2_fft_size,
					manager_ctx->fft_batchlen,
					manager_ctx->avg_factor,
					manager_ctx->soverlap,
					manager_ctx->freq_overlap,
					manager_ctx->monitor_time,
					manager_ctx->min_time_res,
					manager_ctx->window_fun_str
			);
			exit(1);
		} else {
			manager_ctx->min_freq = atol(argv[optind]);
			manager_ctx->max_freq = atol(argv[optind+1]);
		}

	}

	// Initialize sensor contexts
	manager_ctx = (ManagerCTX *) malloc(sizeof(ManagerCTX));
	manager_ctx->dev_index = DEFAULT_DEV_INDEX;
	manager_ctx->clk_off = DEFAULT_CLK_OFF;
	manager_ctx->clk_corr_period = DEFAULT_CLK_CORR_PERIOD;
	manager_ctx->samp_rate = DEFAULT_SAMP_RATE;
	manager_ctx->log2_fft_size = DEFAULT_LOG2_FFT_SIZE;
	manager_ctx->avg_factor = DEFAULT_AVG_FACTOR;
	manager_ctx->soverlap = DEFAULT_SOVERLAP;
	manager_ctx->monitor_time = DEFAULT_MONITOR_TIME;
	manager_ctx->min_time_res = DEFAULT_MIN_TIME_RES;
	manager_ctx->number_of_sample_runs = 0;
	manager_ctx->fft_batchlen = DEFAULT_FFT_BATCHLEN;
	manager_ctx->gain = DEFAULT_GAIN;
	manager_ctx->freq_overlap = DEFAULT_FREQ_OVERLAP;
	manager_ctx->hopping_strategy_str = DEFAULT_HOPPING_STRATEGY_STR;
	manager_ctx->window_fun_str = DEFAULT_WINDOW_FUN_STR;
	THR_initialize(&(manager_ctx->thread), THR_MANAGER);

	// Parse arguments/options and update context
	parse_args(argc, argv);

	freq_corr_ctx = (FrequencyCorrectionCTX *) malloc(sizeof(FrequencyCorrectionCTX));
	freq_corr_ctx->clk_off = manager_ctx->clk_off;
	freq_corr_ctx->dev_index = manager_ctx->dev_index;
	THR_initialize(&(freq_corr_ctx->thread), THR_FREQ_CORR);

	spec_moni_ctx = (SpectrumMonitoringCTX *) malloc(sizeof(SpectrumMonitoringCTX));
	spec_moni_ctx->min_freq = manager_ctx->min_freq;
	spec_moni_ctx->max_freq = manager_ctx->max_freq;
	spec_moni_ctx->dev_index = manager_ctx->dev_index;
	spec_moni_ctx->clk_off = manager_ctx->clk_off;
	spec_moni_ctx->samp_rate = manager_ctx->samp_rate;
	spec_moni_ctx->log2_fft_size = manager_ctx->log2_fft_size;
	spec_moni_ctx->avg_factor = manager_ctx->avg_factor;
	spec_moni_ctx->soverlap = manager_ctx->soverlap;
	spec_moni_ctx->monitor_time = manager_ctx->monitor_time;
	spec_moni_ctx->min_time_res = manager_ctx->min_time_res;
	spec_moni_ctx->number_of_sample_runs = manager_ctx->number_of_sample_runs;
	spec_moni_ctx->fft_batchlen = manager_ctx->fft_batchlen;
	spec_moni_ctx->gain = manager_ctx->gain;
	spec_moni_ctx->freq_overlap = manager_ctx->freq_overlap;
	if(strcmp(manager_ctx->hopping_strategy_str, "random") == 0)
		spec_moni_ctx->hopping_strategy_id = RANDOM_HOPPING_STRATEGY;
	else if(strcmp(manager_ctx->hopping_strategy_str, "similarity") == 0)
		spec_moni_ctx->hopping_strategy_id = SIMILARITY_HOPPING_STRATEGY;
	else
		spec_moni_ctx->hopping_strategy_id = SEQUENTIAL_HOPPING_STRATEGY;
	if(strcmp(manager_ctx->window_fun_str, "hanning") == 0)
		spec_moni_ctx->window_fun_id = HANNING_WINDOW;
	else if(strcmp(manager_ctx->window_fun_str, "blackman_harris_4") == 0)
		spec_moni_ctx->window_fun_id = BLACKMAN_HARRIS_WINDOW;
	else
		spec_moni_ctx->window_fun_id = SEQUENTIAL_HOPPING_STRATEGY;
	THR_initialize(&(spec_moni_ctx->thread), THR_SPEC_MONI);

	// Initialize RTL-SDR device and associated lock to guarantee mutual exclusive access
	SDR_initialize(&rtlsdr_dev, spec_moni_ctx->dev_index);
	rtlsdr_mut = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(rtlsdr_mut, NULL);

#if defined(VERBOSE) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[SMAN] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[SMAN] Started.\n");
#endif
#endif

	// Initialize senor arguments
	freq_corr_arg = (FrequencyCorrectionARG *) malloc(sizeof(FrequencyCorrectionARG));
	freq_corr_arg->manager_ctx = manager_ctx;
	freq_corr_arg->freq_corr_ctx = freq_corr_ctx;

	spec_moni_arg = (SpectrumMonitoringARG *) malloc(sizeof(SpectrumMonitoringARG));
	spec_moni_arg->manager_ctx = manager_ctx;
	spec_moni_arg->spec_moni_ctx = spec_moni_ctx;

	// Ctrl-C signal catcher
	signal(SIGINT, terminate);

	// Start sensor threads
	pthread_mutex_lock(freq_corr_ctx->thread->lock);
	pthread_create(freq_corr_ctx->thread->fd, NULL, frequency_correction, freq_corr_arg);
	pthread_cond_wait(manager_ctx->thread->awake, freq_corr_ctx->thread->lock);
	pthread_mutex_unlock(freq_corr_ctx->thread->lock);

	pthread_mutex_lock(spec_moni_ctx->thread->lock);
	pthread_create(spec_moni_ctx->thread->fd, NULL, spectrum_monitoring, spec_moni_arg);
	pthread_cond_wait(manager_ctx->thread->awake, spec_moni_ctx->thread->lock);
	pthread_mutex_unlock(spec_moni_ctx->thread->lock);

	// Managing logic
	int r;
	unsigned int once = 1;
	struct timeval now;
	struct timespec freq_corr_timer;

	pthread_mutex_lock(manager_ctx->thread->lock);

	// Perform initial frequency correction
	pthread_mutex_lock(freq_corr_ctx->thread->lock);
	freq_corr_ctx->clk_off = manager_ctx->clk_off;
	pthread_cond_signal(freq_corr_ctx->thread->awake);
	pthread_mutex_unlock(freq_corr_ctx->thread->lock);

	gettimeofday(&now, NULL);
	freq_corr_timer.tv_sec = now.tv_sec;
	freq_corr_timer.tv_nsec = now.tv_usec * 1000;
	freq_corr_timer.tv_sec += manager_ctx->clk_corr_period;

	while(1) {

		// Manager awaits events
		r = pthread_cond_timedwait(manager_ctx->thread->awake, manager_ctx->thread->lock,
				&freq_corr_timer);

		// Redo frequency correction
		if(r == ETIMEDOUT) {
			pthread_mutex_lock(freq_corr_ctx->thread->lock);
			freq_corr_ctx->clk_off = manager_ctx->clk_off;
			pthread_cond_signal(freq_corr_ctx->thread->awake);
			pthread_mutex_unlock(freq_corr_ctx->thread->lock);

			gettimeofday(&now, NULL);
			freq_corr_timer.tv_sec = now.tv_sec;
			freq_corr_timer.tv_nsec = now.tv_usec * 1000;
			freq_corr_timer.tv_sec += manager_ctx->clk_corr_period;
		}

		// Handle flagged events
		while(manager_ctx->thread->flags) {

			// Frequency correction terminated
			if(manager_ctx->thread->flags & FLAG_FREQ_CORR) {

#if defined(VERBOSE)
				fprintf(stderr, "[SMAN] Frequency correction terminated.\n");
#endif

				// Reset flag
				manager_ctx->thread->flags ^= FLAG_FREQ_CORR;

				// Update manager context
				pthread_mutex_lock(freq_corr_ctx->thread->lock);
				manager_ctx->clk_off = freq_corr_ctx->clk_off;
				pthread_mutex_unlock(freq_corr_ctx->thread->lock);

				// Update frequency monitoring context
				pthread_mutex_lock(spec_moni_ctx->thread->lock);
				spec_moni_ctx->clk_off = manager_ctx->clk_off;
				pthread_mutex_unlock(spec_moni_ctx->thread->lock);
			}

			// Spectrum monitoring terminated
			if(manager_ctx->thread->flags & FLAG_SPEC_MONI) {

#if defined(VERBOSE)
				fprintf(stderr, "[SMAN] Spectrum monitoring terminated.\n");
#endif

				// Reset flag
				manager_ctx->thread->flags ^= FLAG_SPEC_MONI;

				// Terminate when monitoring is done
				terminate(0);
			}
		}

		// Perform frequency monitoring once
		if(once) {
			once = 0;
			pthread_cond_signal(spec_moni_ctx->thread->awake);
		}

	}

	return 0;
}

static void* frequency_correction(void *args) {
	int clk_off;

	//   char buf[100];
	//   FILE *f_kal;
	int enable_temp_sensor = 0;
	char *temp_sensor = NULL;
	const char *temp_sensor_path = "/sys/bus/w1/devices/", *temp_sensor_file = "w1_slave";
	FILE *f_temp = NULL;
	DIR *d_temp_sensor_path, *d_temp_sensor;
	struct dirent *d_temp_sensor_serial;

	FrequencyCorrectionARG *freq_corr_arg;
	ManagerCTX *manager_ctx;
	FrequencyCorrectionCTX *freq_corr_ctx;

	// Parse arguments
	freq_corr_arg = (FrequencyCorrectionARG *) args;
	manager_ctx = (ManagerCTX *) freq_corr_arg->manager_ctx;
	freq_corr_ctx = (FrequencyCorrectionCTX *) freq_corr_arg->freq_corr_ctx;

#if defined(VERBOSE) || defined(VERBOSE_FCOR) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[FCOR] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[FCOR] Started.\n");
#endif
#endif

	// Open temperature sensor file
	if((d_temp_sensor_path = opendir(temp_sensor_path)) != NULL) {
		while((d_temp_sensor_serial = readdir(d_temp_sensor_path)) != NULL) {
			if((strcmp(d_temp_sensor_serial->d_name, ".") != 0) &&
					(strcmp(d_temp_sensor_serial->d_name, "..") != 0)) {
				temp_sensor = realloc(temp_sensor,
						strlen(temp_sensor_path) +
						strlen(d_temp_sensor_serial->d_name) +
						strlen(temp_sensor_file) + 2);
				strcpy(temp_sensor, temp_sensor_path);
				strcat(temp_sensor, d_temp_sensor_serial->d_name);
				strcat(temp_sensor, "/");
				if((d_temp_sensor = opendir(temp_sensor)) != NULL) {
					strcat(temp_sensor, temp_sensor_file);
					if((f_temp = fopen(temp_sensor, "r")) != NULL) {
						enable_temp_sensor = 1;
						fclose(f_temp);
						break;
					}
					closedir(d_temp_sensor);
				}
			}
		}
		closedir(d_temp_sensor_path);
	}
#if defined(VERBOSE) || defined(VERBOSE_FCOR)
	fprintf(stderr, "[FCOR] Temperature sensor %sfound.\n", enable_temp_sensor ? "" : "not ");
#endif

	// Signal manager that we are ready
	pthread_mutex_lock(freq_corr_ctx->thread->lock);
	pthread_cond_signal(manager_ctx->thread->awake);
	while(freq_corr_ctx->thread->is_running) {
		// Await requests
		pthread_cond_wait(freq_corr_ctx->thread->awake, freq_corr_ctx->thread->lock);
		if(!freq_corr_ctx->thread->is_running) break;

		// Read context
		clk_off = freq_corr_ctx->clk_off;
		pthread_mutex_unlock(freq_corr_ctx->thread->lock);

		// Perform frequency correction
#if defined(VERBOSE) || defined(VERBOSE_FCOR)
		fprintf(stderr, "[FCOR] Run frequency correction.\n");
#endif

		//     // TODO: Implement frequency correction based on GSM signals
		//
		//     // Acquire lock to RTL-SDR device, sampling gets blocked
		//     pthread_mutex_lock(rtlsdr_mut);
		//
		//     // Release RTL-SDR device for external correction process
		//     SDR_release(rtlsdr_dev);
		//
		//     f_kal = popen("kal -s GSM900 -g 48.0 -e 0 2> /dev/null | grep -o \"chan:\\s\\+[0-9]\\+\\|power:\\s\\+[0-9]\\+\\.[0-9]\\+\"", "r");
		//     if(f_kal != NULL) {
		//       while(fgets(buf, sizeof(buf), f_kal) != NULL) {
		// 	fprintf(stdout, ">>> %s\n", buf);
		//       }
		//       pclose(f_kal);
		//     } else {
		//       fprintf(stderr, "[FCOR] Failed opening kalibrate-rtl.\n");
		//     }
		//
		//     // Acquire RTL-SDR device
		//     SDR_initialize(&rtlsdr_dev, freq_corr_ctx->dev_index);
		//
		//     // Release lock to RTL-SDR device
		//     pthread_mutex_unlock(rtlsdr_mut);

		// Read temperature measures
		if(enable_temp_sensor && (f_temp = fopen(temp_sensor, "r")) != NULL) {
			char res[4];
			fscanf(f_temp, "%*2x %*2x %*2x %*2x %*2x %*2x %*2x %*2x %*2x %*1c %*s %s", res);
			if(strcmp(res, "YES") == 0) {
				float temp;
				fscanf(f_temp, "%*2x %*2x %*2x %*2x %*2x %*2x %*2x %*2x %*2x %*2c %f", &temp);
				temp /= 1000.f;
#if defined(VERBOSE) || defined(VERBOSE_FCOR)
				fprintf(stderr, "[FCOR] Current temperature: %.3f degree Celsius.\n", temp);
#endif
			}
			fclose(f_temp);

			// TODO: Implement model to predict frequency error based on temperature variations
		}

		// Write context
		pthread_mutex_lock(freq_corr_ctx->thread->lock);
		freq_corr_ctx->clk_off = clk_off;

		// Signal manager that frequency correction completed
		pthread_mutex_lock(manager_ctx->thread->lock);
		manager_ctx->thread->flags |= FLAG_FREQ_CORR;
		pthread_cond_signal(manager_ctx->thread->awake);
		pthread_mutex_unlock(manager_ctx->thread->lock);
	}
	pthread_mutex_unlock(freq_corr_ctx->thread->lock);

	free(temp_sensor);

#if defined(VERBOSE) || defined(VERBOSE_FCOR)
	fprintf(stderr, "[FCOR] Terminated.\n");
#endif

	pthread_exit(NULL);
}

static void* spectrum_monitoring(void *args) {

	SpectrumMonitoringARG *spec_moni_arg;
	ManagerCTX *manager_ctx;
	SpectrumMonitoringCTX *spec_moni_ctx;

	/*! Monitoring Manager
	 */
	void monitoring_logic() {
		int q_size;

		unsigned int min_freq, max_freq;
		unsigned int samp_rate;
		unsigned int log2_fft_size, avg_factor, soverlap;
		unsigned int monitor_time, min_time_res, fft_batchlen;
		unsigned int number_of_sample_runs, measurements_left;

		int clk_off;
		float gain, freq_overlap;
		int hopping_strategy_id;
		int window_fun_id;
		void (*hopping_strategy)() = NULL;

		unsigned int length = 0, full_length = 0;
		unsigned int *samp_rates = NULL;
		unsigned int *log2_fft_sizes = NULL;
		unsigned int *avg_factors = NULL;
		unsigned int *soverlaps = NULL;
		unsigned int *center_freqs = NULL, *full_center_freqs = NULL;
		float *freq_overlaps = NULL;
		window_fun_ptr_t *window_funs = NULL;

#if defined(VERBOSE) || defined(VERBOSE_STRATEGY)
		long long cnt_total = 0, cnt_skipped = 0;
#endif


		// History hash table
		pthread_mutex_t *hist_htable_mut;

		time_t start_t, current_t, prev_t;


		SamplingWindowingCTX *samp_wind_ctx = NULL;
		SamplingWindowingARG *samp_wind_arg = NULL;

		Queue                *q_fft = NULL;
		FFTCTX               *fft_ctx;
		FFTARG               *fft_arg;

		Queue                *q_avg = NULL;
		AveragingCTX         *avg_ctx = NULL;
		AveragingARG         *avg_arg = NULL;

		Queue                *q_dump = NULL;
		DumpingCTX           *dump_ctx;
		DumpingARG           *dump_arg;


		/*! Rectangular Window
		 */
		 float rectangular_window(int n, int N) {
			 return 1.0;
		 }

		 /*! Hanning Window
		  */
		 float hanning_window(int n, int N) {
			 return 0.5 * (1- cos(2*M_PI*n / (N-1)));
		 }

		 /*! 4-Term  Blackman-Harris Window
		  */
		 float blackman_harris_4_window(int n, int N) {
			 const float a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
			 return a0 - a1*cos(2*M_PI*n / (N-1)) + a2*cos(4*M_PI*n / (N-1)) - a3*cos(6*M_PI*n / (N-1));
		 }

		 /*! Sequential Hopping Strategy
		  *
		  * In the sequential hopping strategy, each sweep tunes to the very same sequence of center
		  * frequencies, using a fixed sampling rate, FFT length, averaging factor, compression level and
		  * windowing function. Therefore, the sweeping parameters have only to be calculated once. The
		  * clock error is updated for each sweep, using its latest estimate.
		  */
		 void sequential_hopping_strategy() {
			 int i;

			 // Update clk_off
			 clk_off = spec_moni_ctx->clk_off;

			 // Calculate hopping parameters once
			 if(length <= 0) {
				 unsigned int freq_step;

				 // Set hopping strategy dependent callbacks
				 if(samp_wind_ctx != NULL) samp_wind_ctx->callback = NULL;
				 if(fft_ctx != NULL) fft_ctx->callback = NULL;
				 if(avg_ctx != NULL) avg_ctx->callback = NULL;

				 freq_step = (1 - freq_overlap) * samp_rate;
				 length = (max_freq - min_freq + 1e6) / freq_step;

				 // Sampling rates
				 samp_rates = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) samp_rates[i] = samp_rate;

				 // FFT sizes
				 log2_fft_sizes = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) log2_fft_sizes[i] = log2_fft_size;

				 // Averaging factors
				 avg_factors = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) avg_factors[i] = avg_factor;

				 // Segment overlaps
				 soverlaps = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) soverlaps[i] = soverlap;

				 // Center frequencies
				 center_freqs = (unsigned int *) malloc(length*sizeof(unsigned int));
				 center_freqs[0] = min_freq + 0.5 * freq_step;
				 for(i=1; i<length; ++i) center_freqs[i] = center_freqs[i-1] + freq_step;

				 // Frequency overlaps
				 freq_overlaps = (float *) malloc(length*sizeof(float));
				 for(i=0; i<length; ++i) freq_overlaps[i] = freq_overlap;

				 // Windowing functions
				 window_fun_ptr_t window_fun = NULL;
				 if(window_fun_id == HANNING_WINDOW) window_fun = hanning_window;
				 else if(window_fun_id == BLACKMAN_HARRIS_WINDOW) window_fun = blackman_harris_4_window;
				 else window_fun = rectangular_window;
				 window_funs = (window_fun_ptr_t *) malloc(length*sizeof(window_fun_ptr_t));
				 for(i=0; i<length; ++i) window_funs[i] = window_fun;
			 }

		 }

		 /*! Random Hopping Strategy
		  *
		  * Tune to a random center frequency. The frequency resolution introduced by the FFT determines
		  * the resolution of center frequencies to tune too. So we can think of the frequency range of
		  * inspection being divided into a sequence of bins at resolution distance, to which the
		  * strategy hops with equal probability.
		  *
		  */
		 void random_hopping_strategy() {
			 int i;
			 unsigned int freq_step;
			 unsigned int min_f, max_f;
			 unsigned int resolution;

			 // Calculate hopping parameters once
			 if(length <= 0) {

				 // Set hopping strategy dependent callbacks
				 if(samp_wind_ctx != NULL) samp_wind_ctx->callback = NULL;
				 if(fft_ctx != NULL) fft_ctx->callback = NULL;
				 if(avg_ctx != NULL) avg_ctx->callback = NULL;

				 // Initialize random number generator
				 srand((unsigned int) time(NULL));

				 freq_step = (1 - freq_overlap) * samp_rate;
				 length = (max_freq - min_freq + 1e6) / freq_step;

				 // Sampling rates
				 samp_rates = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) samp_rates[i] = samp_rate;

				 // FFT sizes
				 log2_fft_sizes = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) log2_fft_sizes[i] = log2_fft_size;

				 // Averaging factors
				 avg_factors = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) avg_factors[i] = avg_factor;

				 // Segment overlaps
				 soverlaps = (unsigned int *) malloc(length*sizeof(unsigned int));
				 for(i=0; i<length; ++i) soverlaps[i] = soverlap;

				 // Center frequencies
				 center_freqs = (unsigned int *) malloc(length*sizeof(unsigned int));

				 // Frequency overlaps
				 freq_overlaps = (float *) malloc(length*sizeof(float));
				 for(i=0; i<length; ++i) freq_overlaps[i] = freq_overlap;

				 // Windowing functions
				 window_fun_ptr_t window_fun = NULL;
				 if(window_fun_id == HANNING_WINDOW) window_fun = hanning_window;
				 else if(window_fun_id == BLACKMAN_HARRIS_WINDOW) window_fun = blackman_harris_4_window;
				 else window_fun = rectangular_window;
				 window_funs = (window_fun_ptr_t *) malloc(length*sizeof(window_fun_ptr_t));
				 for(i=0; i<length; ++i) window_funs[i] = window_fun;
			 }

			 // Tune to purely random center frequencies, at the resolution introduced by the FFT length
			 freq_step = (1 - freq_overlap) * samp_rate;
			 resolution = samp_rate / (1 << log2_fft_size);
			 // Round down for min_f, up for max_f, so it's guaranteed we inspect the full span
			 min_f = (min_freq + 0.5*freq_step) / resolution;
			 max_f = (max_freq - 0.5*freq_step + resolution) / resolution;
			 for(i=0; i<length; ++i) center_freqs[i] = (min_f + (rand() % (max_f-min_f+1))) * resolution;
		 }

		 /*!
		  * Similarity based hopping strategy
		  *
		  * NOTE: For the history to work, we need the parameters to stay constant over time. Sampling
		  * rates, FFT lengths, averaging factors, compression levels, frequency overlaps and widowing
		  * functions don't change over time and are the same for each step. The only thing that changes
		  * is the sequence of center frequencies to tune to.
		  */

		 const float similarity_reduction = 1.0005;

		 typedef struct {
			 int Fc;
			 float similarity;
			 float *previous_signal;
			 UT_hash_handle hh;
		 } SimilarityHistEntry;

		 SimilarityHistEntry *similarity_hist_htable;

		 void similarity_fft_callback(Item *iout) {

			 int                 key, key_size, signal_len;
			 float               *signal, *filtered_signal;
			 SimilarityHistEntry *hentry;

			 // EMA degree of weighting decrease
			 const float alpha_filter = 0.75f;
			 const float alpha_recursive = 0.75f;

			 /*!
			  * Exponential moving average (EMA) IIR filter
			  *
			  * \param N Signal length
			  * \param x Input signal
			  * \param y Output signal
			  */

			 void ema_filter(const int N, const float *x, float *y) {
				 int i;
				 if(N <= 0) return;
				 y[0] = x[0];
				 for(i=1; i<N; ++i) y[i] = alpha_filter * x[i] + (1-alpha_filter) * y[i-1];
			 }

			 /*!
			  * Exponential moving average (EMA) recursive IIR filter
			  *
			  * \param s EMA value
			  * \param y value
			  */
			 void ema_recursive(float *s, const float y) {
				 *s = alpha_recursive * y + (1-alpha_recursive) * (*s);
			 }

			 /*!
			  * Spectrum similarity
			  *
			  * Expects both signals 'x' and 'y' to be of length 'N'.
			  */
			 float similarity_estimation(const int N, const float *x, const float *y) {
				 int          m, n;
				 double       t, b = 0;
				 double       sum_x_square = 0, sum_y_square = 0, norm;
				 float        res, s = 0;

				 const int    M = 2, p = 2;
				 const float  c = 0.8;

				 // Cross-correlation
				 double xcorr(const float *x, const float *y, const unsigned int N, int m) {
					 int n;
					 double res = 0;
					 // Cross-correlation
					 if(m<0) {
						 m = -m;
						 for(n=0; n<N-m; ++n) res += (y[n+m]*x[n]);
					 } else {
						 for(n=0; n<N-m; ++n) res += (x[n+m]*y[n]);
					 }
					 return res;
				 }

				 // Shift degradation
				 float Sp(int m) {
					 if(m < 0) m = -m;
					 return (float) (pow((((float) -m) / ((float) M) + 1), p));
				 }

				 // Coefficient-based normalization
				 for(n=0; n<N; ++n) {
					 sum_x_square += x[n]*x[n];
					 sum_y_square += y[n]*y[n];
				 }
				 norm = 1.0 / (sqrt(sum_x_square) * sqrt(sum_y_square));

				 // Shifting
				 for(m=-M; m<=M; ++m) {
					 t = norm * xcorr(x, y, N, m);
					 if(t > b) {
						 s = m;
						 b = t;
					 }
				 }
				 // Similarity measure in percent
				 res = (c*b + (1-c)*Sp(s))*100;

				 return res;
			 }

			 key = iout->Fc;
			 key_size = sizeof(int);
			 signal_len = 1 << iout->log2_fft_size;
			 signal = iout->samples;
			 filtered_signal = (float *) malloc(signal_len * sizeof(float));
			 hentry = NULL;

			 // Filter signal
			 ema_filter(signal_len, signal, filtered_signal);

			 // Similarity estimation
			 pthread_mutex_lock(hist_htable_mut);
			 HASH_FIND(hh, similarity_hist_htable, &key, key_size, hentry);
			 // Entry with requested key found in history
			 if(hentry != NULL) {
				 float s;

				 // Estimate similarity to previous signal
				 s = similarity_estimation(signal_len, hentry->previous_signal, filtered_signal);
				 // Update EMA similarity
				 ema_recursive(&(hentry->similarity), s);

#if defined(MEASURE_SIMILARITY)
				 // File format: TimeSecs, TimeMicrosecs, CenterFrequency, LatestSimilarity, EMASimilarity
				 FILE *f_stat_similarity = fopen("dat/stats/f_stat_similarity.dat", "a");
				 fprintf(f_stat_similarity, "%u, %u, %u, %.5f, %.5f\n", iout->Ts_sec, iout->Ts_usec, key, s, hentry->similarity);
				 fclose(f_stat_similarity);
#endif

				 // Keep latest signal in history
				 free(hentry->previous_signal);
				 hentry->previous_signal = filtered_signal;
			 }
			 // Entry with requested key not found in history
			 else {
				 // Initialize and add new entry to history
				 hentry = (SimilarityHistEntry *) malloc(sizeof(SimilarityHistEntry));
				 hentry->Fc = key;
				 hentry->similarity = 0.f;
				 hentry->previous_signal = filtered_signal;
				 HASH_ADD(hh, similarity_hist_htable, Fc, key_size, hentry);
			 }
			 pthread_mutex_unlock(hist_htable_mut);

		 }

		 void similarity_hopping_strategy() {
			 int i;

			 // Calculate full hopping parameters once
			 if(full_length <= 0) {
				 unsigned int freq_step;

				 // Set hopping strategy dependent callbacks
				 if(samp_wind_ctx != NULL) samp_wind_ctx->callback = NULL;
				 if(fft_ctx != NULL) fft_ctx->callback = similarity_fft_callback;
				 if(avg_ctx != NULL) avg_ctx->callback = NULL;

				 // Initialize random number generator
				 srand((unsigned int) time(NULL));

				 freq_step = (1 - freq_overlap) * samp_rate;
				 full_length = (max_freq - min_freq + 1e6) / freq_step;

				 // Sampling rates
				 samp_rates = (unsigned int *) malloc(full_length*sizeof(unsigned int));
				 for(i=0; i<full_length; ++i) samp_rates[i] = samp_rate;

				 // FFT sizes
				 log2_fft_sizes = (unsigned int *) malloc(full_length*sizeof(unsigned int));
				 for(i=0; i<full_length; ++i) log2_fft_sizes[i] = log2_fft_size;

				 // Averaging factors
				 avg_factors = (unsigned int *) malloc(full_length*sizeof(unsigned int));
				 for(i=0; i<full_length; ++i) avg_factors[i] = avg_factor;

				 // Segment overlaps
				 soverlaps = (unsigned int *) malloc(full_length*sizeof(unsigned int));
				 for(i=0; i<full_length; ++i) soverlaps[i] = soverlap;

				 // Center frequencies
				 full_center_freqs = (unsigned int *) malloc(full_length*sizeof(unsigned int));
				 full_center_freqs[0] = min_freq + 0.5 * freq_step;
				 for(i=1; i<full_length; ++i) full_center_freqs[i] = full_center_freqs[i-1] + freq_step;
				 center_freqs = (unsigned int *) malloc(full_length*sizeof(unsigned int));

				 // Frequency overlaps
				 freq_overlaps = (float *) malloc(full_length*sizeof(float));
				 for(i=0; i<full_length; ++i) freq_overlaps[i] = freq_overlap;

				 // Windowing functions
				 window_fun_ptr_t window_fun = NULL;
				 if(window_fun_id == HANNING_WINDOW) window_fun = hanning_window;
				 else if(window_fun_id == BLACKMAN_HARRIS_WINDOW) window_fun = blackman_harris_4_window;
				 else window_fun = rectangular_window;
				 window_funs = (window_fun_ptr_t *) malloc(full_length*sizeof(window_fun_ptr_t));
				 for(i=0; i<full_length; ++i) window_funs[i] = window_fun;

			 }

			 size_t              key_size = sizeof(unsigned int);
			 unsigned int        key, cnt = 0;
			 SimilarityHistEntry *hentry;

			 // Decide on which center frequencies to tune to in next sweep
			 for(i=0; i<full_length; ++i) {
				 key = full_center_freqs[i];
				 hentry = NULL;

				 pthread_mutex_lock(hist_htable_mut);
				 HASH_FIND(hh, similarity_hist_htable, &key, key_size, hentry);
				 // Entry with requested key found in history
				 if(hentry != NULL) {

					 // Probabilistic tuning
					 int probabilistic_tun(float similarity) {
						 // Get random number in {0.0,100.0{
						 float r = (rand() % 1000) / 10.f;
						 // Map similarity to probability
						 float p = 0.0001f*expf(logf(1000000.f)*similarity/100.f);
						 return r >= p;
					 }

					 // Tune to center frequency with probability dependent on similarity
					 if(probabilistic_tun(hentry->similarity)) {
						 // Reinspect
						 center_freqs[cnt++] = key;
					 } else {
						 // Skip
						 hentry->similarity /= similarity_reduction;
					 }
				 }
				 // Entry with requested key not found in history
				 else center_freqs[cnt++] = key;
				 pthread_mutex_unlock(hist_htable_mut);
			 }
			 length = cnt;
		 }

		 // Read spectrum monitoring context
		 min_freq = spec_moni_ctx->min_freq;
		 max_freq = spec_moni_ctx->max_freq;
		 clk_off = spec_moni_ctx->clk_off;
		 samp_rate = spec_moni_ctx->samp_rate;
		 log2_fft_size = spec_moni_ctx->log2_fft_size;
		 avg_factor = spec_moni_ctx->avg_factor;
		 soverlap = spec_moni_ctx->soverlap;
		 monitor_time = spec_moni_ctx->monitor_time;
		 min_time_res = spec_moni_ctx->min_time_res;
		 number_of_sample_runs = spec_moni_ctx->number_of_sample_runs;
		 measurements_left = number_of_sample_runs;
		 fft_batchlen = spec_moni_ctx->fft_batchlen;
		 gain = spec_moni_ctx->gain;
		 freq_overlap = spec_moni_ctx->freq_overlap;
		 if(spec_moni_ctx->hopping_strategy_id == RANDOM_HOPPING_STRATEGY)
			 hopping_strategy = random_hopping_strategy;
		 else if(spec_moni_ctx->hopping_strategy_id == SIMILARITY_HOPPING_STRATEGY)
			 hopping_strategy = similarity_hopping_strategy;
		 else
			 hopping_strategy = sequential_hopping_strategy;
		 hopping_strategy_id = spec_moni_ctx->hopping_strategy_id;
		 window_fun_id = spec_moni_ctx->window_fun_id;

		 // Initialize history
		 hist_htable_mut = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
		 pthread_mutex_init(hist_htable_mut, NULL);
		 similarity_hist_htable = NULL;

		 // Initialize signal processing queues
		 q_size = MIN(10*fft_batchlen, 100);
		 q_fft = QUE_initialize(q_size);
		 q_avg = QUE_initialize(q_size);
		 q_dump = QUE_initialize(q_size);

		 // Initialize signal processing contexts
		 samp_wind_ctx = (SamplingWindowingCTX *) malloc(sizeof(SamplingWindowingCTX));
		 samp_wind_ctx->gain = gain;
		 samp_wind_ctx->hopping_strategy_id = hopping_strategy_id;
		 samp_wind_ctx->window_fun_id = window_fun_id;
		 THR_initialize(&(samp_wind_ctx->thread), THR_SAMP_WIND);

		 fft_ctx = (FFTCTX *) malloc(sizeof(FFTCTX));
		 fft_ctx->fft_batchlen = fft_batchlen;
		 THR_initialize(&(fft_ctx->thread), THR_FFT);

		 avg_ctx = (AveragingCTX *) malloc(sizeof(AveragingCTX));
		 THR_initialize(&(avg_ctx->thread), THR_AVG);

		 dump_ctx = (DumpingCTX *) malloc(sizeof(DumpingCTX));
		 THR_initialize(&(dump_ctx->thread), THR_DUMP);

		 // Initialize signal processing arguments
		 samp_wind_arg = (SamplingWindowingARG *) malloc(sizeof(SamplingWindowingARG));
		 samp_wind_arg->spec_moni_ctx = spec_moni_ctx;
		 samp_wind_arg->samp_wind_ctx = samp_wind_ctx;
		 samp_wind_arg->qin = NULL;
		 samp_wind_arg->qsout_cnt = 1;
		 samp_wind_arg->qsout = (Queue **) malloc(samp_wind_arg->qsout_cnt*sizeof(Queue *));
		 samp_wind_arg->qsout[0] = q_fft;

		 fft_arg = (FFTARG *) malloc(sizeof(FFTARG));
		 fft_arg->spec_moni_ctx = spec_moni_ctx;
		 fft_arg->fft_ctx = fft_ctx;
		 fft_arg->qin = q_fft;
		 fft_arg->qsout_cnt = 1;
		 fft_arg->qsout = (Queue **) malloc(fft_arg->qsout_cnt*sizeof(Queue *));
		 fft_arg->qsout[0] = q_avg;

		 avg_arg = (AveragingARG *) malloc(sizeof(AveragingARG));
		 avg_arg->spec_moni_ctx = spec_moni_ctx;
		 avg_arg->avg_ctx = avg_ctx;
		 avg_arg->qin = q_avg;
		 avg_arg->qsout_cnt = 1;
		 avg_arg->qsout = (Queue **) malloc(avg_arg->qsout_cnt*sizeof(Queue *));
		 avg_arg->qsout[0] = q_dump;

		 dump_arg = (DumpingARG *) malloc(sizeof(DumpingARG));
		 dump_arg->qin = q_dump;
		 dump_arg->qsout_cnt = 0;
		 dump_arg->qsout = (Queue **) malloc(dump_arg->qsout_cnt*sizeof(Queue *));

		 // Start signal processing threads
		 pthread_mutex_lock(samp_wind_ctx->thread->lock);
		 pthread_create(dump_ctx->thread->fd, NULL, dumping, dump_arg);
		 pthread_create(avg_ctx->thread->fd, NULL, averaging, avg_arg);
		 pthread_create(fft_ctx->thread->fd, NULL, fft, fft_arg);
		 pthread_create(samp_wind_ctx->thread->fd, NULL, sampling_windowing, samp_wind_arg);
		 pthread_cond_wait(spec_moni_ctx->thread->awake, samp_wind_ctx->thread->lock);
		 pthread_mutex_unlock(samp_wind_ctx->thread->lock);
		 spec_moni_ctx->thread->flags |= FLAG_SAMP_WIND;


		 // START SPECTRUM MONITORING
		 time(&start_t);
		 prev_t = 0;
		 while(spec_moni_ctx->thread->is_running) {

			 // Handle flagged events
			 while(spec_moni_ctx->thread->flags) {

				 // Sampling windowing terminated
				 if(spec_moni_ctx->thread->flags & FLAG_SAMP_WIND) {

#if defined(VERBOSE)
					 fprintf(stderr, "[FMON] Sampling windowing terminated.\n");
#endif

					 // Reset flag
					 spec_moni_ctx->thread->flags ^= FLAG_SAMP_WIND;

					 // Terminate after a certain number of runs
					 if((number_of_sample_runs != 0) && (measurements_left == 0))
						 goto EXIT_SPECTRUM_MONITORING;
					 measurements_left--;

					 // Terminate after monitor_time seconds when monitor_time > 0
					 time(&current_t);
					 if(monitor_time > 0 && difftime(current_t, start_t) > monitor_time)
						 goto EXIT_SPECTRUM_MONITORING;

					 // Enforce minimal time resolution
					 while(1) {
						 if(difftime(current_t, prev_t) >= min_time_res) {
							 prev_t = current_t;
							 break;
						 }
						 time(&current_t);
					 }

					 // Hopping strategy
					 hopping_strategy();

					 // Update sampling windowing context
					 pthread_mutex_lock(samp_wind_ctx->thread->lock);
					 samp_wind_ctx->clk_off = clk_off;
					 samp_wind_ctx->length = length;
					 samp_wind_ctx->samp_rates = samp_rates;
					 samp_wind_ctx->log2_fft_sizes = log2_fft_sizes;
					 samp_wind_ctx->avg_factors = avg_factors;
					 samp_wind_ctx->soverlaps = soverlaps;
					 samp_wind_ctx->center_freqs = center_freqs;
					 samp_wind_ctx->freq_overlaps = freq_overlaps;
					 samp_wind_ctx->window_funs = window_funs;
					 samp_wind_ctx->hopping_strategy_id = hopping_strategy_id;
					 samp_wind_ctx->window_fun_id = window_fun_id;

					 // Start sampling windowing
					 pthread_cond_signal(samp_wind_ctx->thread->awake);
					 pthread_mutex_unlock(samp_wind_ctx->thread->lock);

				 }

			 }

			 // Await events
			 pthread_cond_wait(spec_moni_ctx->thread->awake, spec_moni_ctx->thread->lock);
			 if(!spec_moni_ctx->thread->is_running) break;

		 }
		 EXIT_SPECTRUM_MONITORING:
		 // END SPECTRUM MONITORING

#if defined(VERBOSE) || defined(VERBOSE_STRATEGY)
		 if(hopping_strategy == similarity_hopping_strategy)
			 fprintf(stderr, "[AVG ] Skipped:\t\t%.1f%%\n", (cnt_skipped * 100.f) / cnt_total);
#endif

		 /*
		  * NOTE: We indicate the sampling/windowing thread to terminate. This thread will finish the
		  * current sweep and put all remaining items to its output queue. Afterwards the sampling/
		  * windowing thread puts a special termination item to its output queue, signal the FFT
		  * thread that no further data will arrive to its input queue. Once processed all items, the
		  * FFT thread will pass the termination item to its output queue, and so on. This allows as
		  * to flush remaining data in the queueing system, before terminating.
		  */

		 // Ask signal processing threads to terminate
		 pthread_mutex_lock(samp_wind_ctx->thread->lock);
		 samp_wind_ctx->thread->is_running = 0;
		 pthread_mutex_unlock(samp_wind_ctx->thread->lock);

		 // Awake signal processing threads when sleeping
		 pthread_cond_signal(samp_wind_ctx->thread->awake);

		 // Join signal processing threads
		 pthread_mutex_unlock(spec_moni_ctx->thread->lock);
		 pthread_join(*(samp_wind_ctx->thread->fd), NULL);
		 pthread_join(*(fft_ctx->thread->fd), NULL);
		 pthread_join(*(avg_ctx->thread->fd), NULL);
		 pthread_join(*(dump_ctx->thread->fd), NULL);
		 pthread_mutex_lock(spec_moni_ctx->thread->lock);

		 // Free output queues
		 free(samp_wind_arg->qsout);
		 free(fft_arg->qsout);
		 free(avg_arg->qsout);
		 free(dump_arg->qsout);

		 // Free signal processing arguments
		 free(samp_wind_arg);
		 free(fft_arg);
		 free(avg_arg);
		 free(dump_arg);

		 // Free signal processing contexts
		 THR_release(samp_wind_ctx->thread);
		 free(samp_wind_ctx);

		 THR_release(fft_ctx->thread);
		 free(fft_ctx);

		 THR_release(avg_ctx->thread);
		 free(avg_ctx);

		 THR_release(dump_ctx->thread);
		 free(dump_ctx);


		 // Free signal processing queues
		 QUE_release(q_fft);
		 QUE_release(q_avg);
		 QUE_release(q_dump);

		 SimilarityHistEntry *similarity_centry, *similarity_tentry;
		 HASH_ITER(hh, similarity_hist_htable, similarity_centry, similarity_tentry) {
			 HASH_DELETE(hh, similarity_hist_htable, similarity_centry);
			 free(similarity_centry->previous_signal);
			 free(similarity_centry);
		 }

		 pthread_mutex_destroy(hist_htable_mut);
		 free(hist_htable_mut);

		 free(samp_rates);
		 free(log2_fft_sizes);
		 free(avg_factors);
		 free(soverlaps);
		 free(center_freqs);
		 free(full_center_freqs);
		 free(freq_overlaps);
		 free(window_funs);
	}


	// Parse arguments
	spec_moni_arg = (SpectrumMonitoringARG *) args;
	manager_ctx = (ManagerCTX *) spec_moni_arg->manager_ctx;
	spec_moni_ctx = (SpectrumMonitoringCTX *) spec_moni_arg->spec_moni_ctx;

#if defined(VERBOSE) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[SMON] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[SMON] Started.\n");
#endif
#endif

	// Signal manager that we are ready
	pthread_mutex_lock(spec_moni_ctx->thread->lock);
	pthread_cond_signal(manager_ctx->thread->awake);
	while(spec_moni_ctx->thread->is_running) {
		// Await spectrum monitoring job
		pthread_cond_wait(spec_moni_ctx->thread->awake, spec_moni_ctx->thread->lock);
		if(!spec_moni_ctx->thread->is_running) break;

		// Monitoring Logic
		monitoring_logic();

		// Signal manager that spectrum monitoring job completed
		pthread_mutex_lock(manager_ctx->thread->lock);
		manager_ctx->thread->flags |= FLAG_SPEC_MONI;
		pthread_cond_signal(manager_ctx->thread->awake);
		pthread_mutex_unlock(manager_ctx->thread->lock);
	}
	pthread_mutex_unlock(spec_moni_ctx->thread->lock);

#if defined(VERBOSE)
	fprintf(stderr, "[SMON] Terminated.\n");
#endif

	pthread_exit(NULL);
}

static void* sampling_windowing(void *args) {
	int i, j, l;
	uint8_t *iq_buf = NULL;
	struct timeval tv;
	unsigned int slen = 0;

	float gain;
	unsigned int prev_length = 0, length;
	unsigned int samp_rate, *samp_rates = NULL, prev_samp_rate = 0;
	unsigned int prev_fft_size = 0, fft_size, *log2_fft_sizes = NULL;
	unsigned int prev_avg_factor = 0, avg_factor, *avg_factors = NULL;
	unsigned int prev_soverlap = 0, soverlap, *soverlaps = NULL;
	unsigned int center_freq, *center_freqs = NULL, prev_center_freq = 0;
	int hopping_strategy_id;
	int window_fun_id;
	int clk_off;
	float freq_overlap, *freq_overlaps = NULL;
	window_fun_ptr_t window_fun, *window_funs = NULL;

	SamplingWindowingARG *samp_wind_arg;
	SpectrumMonitoringCTX *spec_moni_ctx;
	SamplingWindowingCTX *samp_wind_ctx;

	size_t k, qsout_cnt;
	Queue **qsout;
	Item  *iout, *nout;

#if defined(MEASURE_SAWI)
	struct timespec tstartA = {0,0}, tendA = {0,0};
	struct timespec tstartB = {0,0}, tendB = {0,0};
	FILE *f_stat_sawi_sweep = fopen("dat/stats/f_stat_sawi_sweep.dat", "w");
	FILE *f_stat_sawi_set_sample_rate = fopen("dat/stats/f_stat_sawi_set_sample_rate.dat", "w");
	FILE *f_stat_sawi_retune = fopen("dat/stats/f_stat_sawi_retune.dat", "w");
	FILE *f_stat_sawi_read = fopen("dat/stats/f_stat_sawi_read.dat", "w");
#endif

	// Parse arguments
	samp_wind_arg = (SamplingWindowingARG *) args;
	spec_moni_ctx = (SpectrumMonitoringCTX *) samp_wind_arg->spec_moni_ctx;
	samp_wind_ctx = (SamplingWindowingCTX *) samp_wind_arg->samp_wind_ctx;
	qsout = (Queue **) samp_wind_arg->qsout;
	qsout_cnt = samp_wind_arg->qsout_cnt;

	// Fixed parameters
	gain = samp_wind_ctx->gain;
	hopping_strategy_id = samp_wind_ctx->hopping_strategy_id;
	window_fun_id = samp_wind_ctx->window_fun_id;

	// Acquire lock to RTL-SDR device
	pthread_mutex_lock(rtlsdr_mut);

	// Set RTL-SDR device's gain
	SDR_set_gain(rtlsdr_dev, gain);

	// Release lock to RTL-SDR device
	pthread_mutex_unlock(rtlsdr_mut);

#if defined(VERBOSE) || defined(VERBOSE_SAWI) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[SAWI] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[SAWI] Started.\n");
#endif
#endif

	// Signal monitoring logic that we are ready
	pthread_mutex_lock(samp_wind_ctx->thread->lock);
	pthread_cond_signal(spec_moni_ctx->thread->awake);
	while(samp_wind_ctx->thread->is_running) {
		// Await requests
		pthread_cond_wait(samp_wind_ctx->thread->awake, samp_wind_ctx->thread->lock);
		if(!samp_wind_ctx->thread->is_running) break;

#if defined(VERBOSE) || defined(VERBOSE_SAWI)
		fprintf(stderr, "[SAWI] Request.\n");
#endif

		// Read context
		clk_off = samp_wind_ctx->clk_off;
		length = samp_wind_ctx->length;
		if(prev_length != length) {
			samp_rates = (unsigned int *) realloc(samp_rates, length*sizeof(unsigned int));
			log2_fft_sizes = (unsigned int *) realloc(log2_fft_sizes, length*sizeof(unsigned int));
			avg_factors = (unsigned int *) realloc(avg_factors, length*sizeof(unsigned int));
			soverlaps = (unsigned int *) realloc(soverlaps, length*sizeof(unsigned int));
			center_freqs = (unsigned int *) realloc(center_freqs, length*sizeof(unsigned int));
			freq_overlaps = (float *) realloc(freq_overlaps, length*sizeof(float));
			window_funs = (window_fun_ptr_t *) realloc(window_funs, length*sizeof(window_fun_ptr_t));
			prev_length = length;
		}
		for(i=0; i<length; ++i) {
			samp_rates[i] = samp_wind_ctx->samp_rates[i];
			log2_fft_sizes[i] = samp_wind_ctx->log2_fft_sizes[i];
			avg_factors[i] = samp_wind_ctx->avg_factors[i];
			soverlaps[i] = samp_wind_ctx->soverlaps[i];
			center_freqs[i] = samp_wind_ctx->center_freqs[i];
			freq_overlaps[i] = samp_wind_ctx->freq_overlaps[i];
			window_funs[i] = samp_wind_ctx->window_funs[i];
		}
		pthread_mutex_unlock(samp_wind_ctx->thread->lock);


		// BEGIN SAMPLING WINDOWING

		// Acquire lock to RTL-SDR device
		pthread_mutex_lock(rtlsdr_mut);

		// Correct RTL-SDR device's clock error
		SDR_set_freq_correction(rtlsdr_dev, clk_off);

		// Frequency hopping
#if defined(MEASURE_SAWI)
		TICK(tstartA);
#endif
		for(i=0; i<length; ++i) {

			samp_rate = samp_rates[i];
			fft_size = 1<<log2_fft_sizes[i];
			avg_factor = avg_factors[i];
			soverlap = soverlaps[i];
			center_freq = center_freqs[i];
			freq_overlap = freq_overlaps[i];
			window_fun = window_funs[i];

			// Buffer to store I/Q sample stream
			if(prev_fft_size != fft_size || prev_avg_factor != avg_factor || prev_soverlap != soverlap) {
				slen = ((fft_size-soverlap)*avg_factor+soverlap)*2;
				// NOTE: libusb_bulk_transfer for RTL-SDR seems to crash when not reading multiples of 512
				if(slen % 512 != 0) slen = slen + (512 - (slen % 512));
				iq_buf = (uint8_t *) realloc(iq_buf,slen*sizeof(uint8_t));
				prev_fft_size = fft_size;
				prev_avg_factor = avg_factor;
				prev_soverlap = soverlap;
			}

			// Read I/Q samples from RTL-SDR device
#if defined(MEASURE_SAWI)
			TICK(tstartB);
#endif
			if(samp_rate != prev_samp_rate) {
				SDR_set_sample_rate(rtlsdr_dev, samp_rate);
				prev_samp_rate = samp_rate;
			}
#if defined(MEASURE_SAWI)
			TACK(tstartB, tendB, f_stat_sawi_set_sample_rate);
			TICK(tstartB);
#endif
			if(center_freq != prev_center_freq) {
				SDR_retune(rtlsdr_dev, center_freq);
				prev_center_freq = center_freq;
			}
#if defined(MEASURE_SAWI)
			TACK(tstartB, tendB, f_stat_sawi_retune);
			TICK(tstartB);
#endif
			SDR_read(rtlsdr_dev, iq_buf, slen);

#if defined(MEASURE_SAWI)
			TACK(tstartB, tendB, f_stat_sawi_read);
#endif


			// Segmentation, DC removal and windowing
			gettimeofday(&tv, NULL);
			for(j=0; j<avg_factor; ++j) {

				// Initialize output item
				iout = ITE_init();
				iout->Fc = center_freq;
				iout->Ts_sec = (uint32_t) tv.tv_sec;
				iout->Ts_usec = (uint32_t) tv.tv_usec;
				iout->samples_size = fft_size*2*sizeof(float);
				iout->samples = (float *) malloc(iout->samples_size);
				iout->hopping_strategy_id = hopping_strategy_id;
				iout->window_fun_id = window_fun_id;
				iout->gain = gain;
				iout->samp_rate = samp_rate;
				iout->log2_fft_size = log2_fft_sizes[i];
				iout->avg_index = avg_factor-j;
				iout->avg_factor = avg_factor;
				iout->freq_overlap = freq_overlap;
				iout->soverlap = soverlap;

				// Remove DC bias and apply windowing function
				float i_val, q_val;
				float i_mean = 0, q_mean = 0;
				for(l=0; l<fft_size*2; l=l+2) {
					i_val = (float) iq_buf[l+j*(fft_size-soverlap)*2];
					q_val = (float) iq_buf[l+1+j*(fft_size-soverlap)*2];
					i_mean += i_val;
					q_mean += q_val;
					iout->samples[l] = i_val;
					iout->samples[l+1] = q_val;
				}
				i_mean = i_mean / fft_size;
				q_mean = q_mean / fft_size;
				for(l=0; l<fft_size*2; l=l+2) {
					iout->samples[l] = (iout->samples[l] - i_mean)*window_fun(l, fft_size);
					iout->samples[l+1] = (iout->samples[l+1] - q_mean)*window_fun(l, fft_size);
				}

				// Strategy dependent callback to the monitoring logic
				if(samp_wind_ctx != NULL && samp_wind_ctx->callback != NULL)
					samp_wind_ctx->callback(iout);

				// Single output queue
				if(qsout_cnt == 1) {
					// Wait for output queue not being full
					pthread_mutex_lock(qsout[0]->mut);
					while(qsout[0]->full) {
#if defined(VERBOSE) || defined(VERBOSE_SAWI)
						fprintf(stderr, "[SAWI] Output queue 0 full.\n");
#endif
						pthread_cond_wait(qsout[0]->notFull, qsout[0]->mut);
					}
					QUE_insert(qsout[0], iout);
#if defined(VERBOSE) || defined(VERBOSE_SAWI)
					fprintf(stderr, "[SAWI] Push item %u to output queue 0.\n", iout->avg_index);
#endif
					pthread_mutex_unlock(qsout[0]->mut);
					pthread_cond_signal(qsout[0]->notEmpty);
				}
				// Multiple output queues
				else if(qsout_cnt > 1) {
					// Put item to output queues
					for(k=0; k<qsout_cnt; ++k) {
						// Wait for output queue not being full
						pthread_mutex_lock(qsout[k]->mut);
						while(qsout[k]->full) {
#if defined(VERBOSE) || defined(VERBOSE_SAWI)
							fprintf(stderr, "[SAWI] Output queue %zd full.\n", k);
#endif
							pthread_cond_wait(qsout[k]->notFull, qsout[k]->mut);
						}
						nout = ITE_copy(iout);
						// Write item to output queue
						QUE_insert(qsout[k], nout);
#if defined(VERBOSE) || defined(VERBOSE_SAWI)
						fprintf(stderr, "[SAWI] Push item %u to output queue %zd.\n", nout->avg_index, k);
#endif
						pthread_mutex_unlock(qsout[k]->mut);
						pthread_cond_signal(qsout[k]->notEmpty);
					}
					// Release item
					ITE_free(iout);
				}

			}

		}
#if defined(MEASURE_SAWI)
		TACK(tstartA, tendA, f_stat_sawi_sweep);
#endif

		// Release lock to RTL-SDR device
		pthread_mutex_unlock(rtlsdr_mut);

		// END SAMPLING WINDOWING


#if defined(VERBOSE) || defined(VERBOSE_SAWI)
		fprintf(stderr, "[SAWI] Response.\n");
#endif

		// Write context
		pthread_mutex_lock(samp_wind_ctx->thread->lock);

		// Signal monitoring logic that sampling windowing completed
		pthread_mutex_lock(spec_moni_ctx->thread->lock);
		spec_moni_ctx->thread->flags |= FLAG_SAMP_WIND;
		pthread_cond_signal(spec_moni_ctx->thread->awake);
		pthread_mutex_unlock(spec_moni_ctx->thread->lock);
	}
	pthread_mutex_unlock(samp_wind_ctx->thread->lock);

	// Signal that we are done and no further items will appear in the queues
	for(k=0; k<qsout_cnt; ++k) {
		pthread_mutex_lock(qsout[k]->mut);
		qsout[k]->exit = 1;
		pthread_cond_signal(qsout[k]->notEmpty);
		pthread_cond_signal(qsout[k]->notFull);
		pthread_mutex_unlock(qsout[k]->mut);
	}

#if defined(MEASURE_SAWI)
	fclose(f_stat_sawi_set_sample_rate);
	fclose(f_stat_sawi_retune);
	fclose(f_stat_sawi_read);
#endif

	free(samp_rates);
	free(log2_fft_sizes);
	free(avg_factors);
	free(soverlaps);
	free(center_freqs);
	free(freq_overlaps);
	free(window_funs);
	free(iq_buf);

#if defined(VERBOSE) || defined(VERBOSE_SAWI)
	fprintf(stderr, "[SAWI] Terminated.\n");
#endif

	pthread_exit(NULL);

}

static void* fft(void *args) {
	int i;

	unsigned int prev_log2_fft_size = 0, log2_fft_size, fft_size;
	unsigned int fft_batchlen;
	unsigned int batch_cnt = 0;

	FFTARG *fft_arg;
	FFTCTX *fft_ctx;

	size_t k, qsout_cnt;
	Queue *qin, **qsout;
	Item  *iin, **its, *nout;

	float *IQ_samples, *dB_samples;
	float **batch_in, **batch_out;

	// Parse arguments
	fft_arg = (FFTARG *) args;
	fft_ctx = (FFTCTX *) fft_arg->fft_ctx;
	qin = (Queue *) fft_arg->qin;
	qsout = (Queue **) fft_arg->qsout;
	qsout_cnt = fft_arg->qsout_cnt;

	// Fixed parameters
	fft_batchlen = fft_ctx->fft_batchlen;

	its = (Item **) malloc(fft_batchlen*sizeof(Item *));
	batch_in = (float **) malloc(fft_batchlen*sizeof(float *));
	batch_out = (float **) malloc(fft_batchlen*sizeof(float *));

#if defined(VERBOSE) || defined(VERBOSE_FFT) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[FFT ] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[FFT ] Started.\n");
#endif
#endif

	while(1) {
		// Wait for input queue not being empty
		pthread_mutex_lock(qin->mut);
		while(qin->empty) {
			// No more input is coming to this queue
			if(qin->exit) {
				pthread_mutex_unlock(qin->mut);
				// Process remaining jobs in a smaller batch
				if(batch_cnt > 0) {
					// Reinitialize FFT resources for smaller batch size
					FFT_release();
					FFT_initialize(prev_log2_fft_size, batch_cnt);
					// Perform batched forward FFT
					FFT_forward(batch_in, batch_out);
					// Write items to output queue
					for(i=0; i<batch_cnt; ++i) {
						// Release I/Q samples
						free(batch_in[i]);
						// Magnitude values require half the I/Q size
						its[i]->samples_size /= 2;
						// Store dB samples
						its[i]->samples = batch_out[i];
						// Strategy dependent callback to the monitoring logic
						if(fft_ctx != NULL && fft_ctx->callback != NULL)
							fft_ctx->callback(its[i]);

						// Single output queue
						if(qsout_cnt == 1) {
							// Wait for output queue not being full
							pthread_mutex_lock(qsout[0]->mut);
							while(qsout[0]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
								fprintf(stderr, "[FFT ] Output queue 0 full.\n");
#endif
								pthread_cond_wait(qsout[0]->notFull, qsout[0]->mut);
							}
							QUE_insert(qsout[0], its[i]);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
							fprintf(stderr, "[FFT ] Push item %u to output queue 0.\n", its[i]->avg_index);
#endif
							pthread_mutex_unlock(qsout[0]->mut);
							pthread_cond_signal(qsout[0]->notEmpty);
						}
						// Multiple output queues
						else if(qsout_cnt > 1) {
							// Put item to output queues
							for(k=0; k<qsout_cnt; ++k) {
								// Wait for output queue not being full
								pthread_mutex_lock(qsout[k]->mut);
								while(qsout[k]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
									fprintf(stderr, "[FFT ] Output queue %zd full.\n", k);
#endif
									pthread_cond_wait(qsout[k]->notFull, qsout[k]->mut);
								}
								nout = ITE_copy(its[i]);
								// Write item to output queue
								QUE_insert(qsout[k], nout);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
								fprintf(stderr, "[FFT ] Push item %u to output queue %zd.\n", nout->avg_index, k);
#endif
								pthread_mutex_unlock(qsout[k]->mut);
								pthread_cond_signal(qsout[k]->notEmpty);
							}
							// Release item
							ITE_free(its[i]);
						}

					}
				}
				// Release FFT resources
				FFT_release();
				goto EXIT;
			}
			// Wait for more input coming to this queue
			pthread_cond_wait(qin->notEmpty, qin->mut);
		}

		// Read item from input queue
		iin = (Item *) QUE_remove(qin);
		pthread_mutex_unlock(qin->mut);
		pthread_cond_signal(qin->notFull);

		// Get item's FFT size
		log2_fft_size = iin->log2_fft_size;
		fft_size = 1<<log2_fft_size;

		// I/Q and dB buffers
		IQ_samples = iin->samples;
		dB_samples = (float *) malloc(fft_size*sizeof(float));

#if defined(VERBOSE) || defined(VERBOSE_FFT)
		fprintf(stderr, "[FFT ] Pull item. LOG2FFT:\t%u\n", log2_fft_size);
#endif

		// FFT size changes
		if(prev_log2_fft_size != log2_fft_size) {

			// Process remaining jobs from previous FFT size
			if(batch_cnt > 0) {
				// Reinitialize FFT resources
				FFT_release();
				FFT_initialize(prev_log2_fft_size, batch_cnt);
				// Perform batched forward FFT
				FFT_forward(batch_in, batch_out);
				// Write items to output queue
				for(i=0; i<batch_cnt; ++i) {
					// Release I/Q samples
					free(batch_in[i]);
					// Magnitude values require half the I/Q size
					its[i]->samples_size /= 2;
					// Store dB samples
					its[i]->samples = batch_out[i];
					// Strategy dependent callback to the monitoring logic
					if(fft_ctx != NULL && fft_ctx->callback != NULL)
						fft_ctx->callback(its[i]);

					// Single output queue
					if(qsout_cnt == 1) {
						// Wait for output queue not being full
						pthread_mutex_lock(qsout[0]->mut);
						while(qsout[0]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
							fprintf(stderr, "[FFT ] Output queue 0 full.\n");
#endif
							pthread_cond_wait(qsout[0]->notFull, qsout[0]->mut);
						}
						QUE_insert(qsout[0], its[i]);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
						fprintf(stderr, "[FFT ] Push item %u to output queue 0.\n", its[i]->avg_index);
#endif
						pthread_mutex_unlock(qsout[0]->mut);
						pthread_cond_signal(qsout[0]->notEmpty);
					}
					// Multiple output queues
					else if(qsout_cnt > 1) {
						// Put item to output queues
						for(k=0; k<qsout_cnt; ++k) {
							// Wait for output queue not being full
							pthread_mutex_lock(qsout[k]->mut);
							while(qsout[k]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
								fprintf(stderr, "[FFT ] Output queue %zd full.\n", k);
#endif
								pthread_cond_wait(qsout[k]->notFull, qsout[k]->mut);
							}
							nout = ITE_copy(its[i]);
							// Write item to output queue
							QUE_insert(qsout[k], nout);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
							fprintf(stderr, "[FFT ] Push item %u to output queue %zd.\n", nout->avg_index, k);
#endif
							pthread_mutex_unlock(qsout[k]->mut);
							pthread_cond_signal(qsout[k]->notEmpty);
						}
						// Release item
						ITE_free(its[i]);
					}

				}
				// Reset batch_cnt
				batch_cnt = 0;
			}

			// Change FFT size
			FFT_release();
			FFT_initialize(log2_fft_size, fft_batchlen);
			prev_log2_fft_size = log2_fft_size;
		}

		// Process items in batches
		its[batch_cnt] = iin;
		batch_in[batch_cnt] = IQ_samples;
		batch_out[batch_cnt] = dB_samples;
		if(++batch_cnt >= fft_batchlen) {
			// Perform batched forward FFT
			FFT_forward(batch_in, batch_out);
			// Write items to output queue
			for(i=0; i<fft_batchlen; ++i) {
				// Release I/Q samples
				free(batch_in[i]);
				// Magnitude values require half the I/Q size
				its[i]->samples_size /= 2;
				// Store dB samples
				its[i]->samples = batch_out[i];
				// Strategy dependent callback to the monitoring logic
				if(fft_ctx != NULL && fft_ctx->callback != NULL)
					fft_ctx->callback(its[i]);

				// Single output queue
				if(qsout_cnt == 1) {
					// Wait for output queue not being full
					pthread_mutex_lock(qsout[0]->mut);
					while(qsout[0]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
						fprintf(stderr, "[FFT ] Output queue 0 full.\n");
#endif
						pthread_cond_wait(qsout[0]->notFull, qsout[0]->mut);
					}
					QUE_insert(qsout[0], its[i]);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
					fprintf(stderr, "[FFT ] Push item %u to output queue 0.\n", its[i]->avg_index);
#endif
					pthread_mutex_unlock(qsout[0]->mut);
					pthread_cond_signal(qsout[0]->notEmpty);
				}
				// Multiple output queues
				else if(qsout_cnt > 1) {
					// Put item to output queues
					for(k=0; k<qsout_cnt; ++k) {
						// Wait for output queue not being full
						pthread_mutex_lock(qsout[k]->mut);
						while(qsout[k]->full) {
#if defined(VERBOSE) || defined(VERBOSE_FFT)
							fprintf(stderr, "[FFT ] Output queue %zd full.\n", k);
#endif
							pthread_cond_wait(qsout[k]->notFull, qsout[k]->mut);
						}
						nout = ITE_copy(its[i]);
						// Write item to output queue
						QUE_insert(qsout[k], nout);
#if defined(VERBOSE) || defined(VERBOSE_FFT)
						fprintf(stderr, "[FFT ] Push item %u to output queue %zd.\n", nout->avg_index, k);
#endif
						pthread_mutex_unlock(qsout[k]->mut);
						pthread_cond_signal(qsout[k]->notEmpty);
					}
					// Release item
					ITE_free(its[i]);
				}

			}
			// Reset batch_cnt
			batch_cnt = 0;
		}

	}

	EXIT:

	// Signal that we are done and no further items will appear in the queues
	for(k=0; k<qsout_cnt; ++k) {
		pthread_mutex_lock(qsout[k]->mut);
		qsout[k]->exit = 1;
		pthread_cond_signal(qsout[k]->notEmpty);
		pthread_cond_signal(qsout[k]->notFull);
		pthread_mutex_unlock(qsout[k]->mut);
	}

	free(batch_out);
	free(batch_in);
	free(its);

#if defined(VERBOSE) || defined(VERBOSE_FFT)
	fprintf(stderr, "[FFT ] Terminated.\n");
#endif

	pthread_exit(NULL);

}

static void* averaging(void *args) {
	int i, j;

	unsigned int avg_index;
	unsigned int fft_size;

	AveragingARG *avg_arg;
	AveragingCTX *avg_ctx;

	size_t k, qsout_cnt;
	Queue *qin, **qsout;
	Item  *iin, *iout, *nout;

	// Parse arguments
	avg_arg = (AveragingARG *) args;
	avg_ctx = (AveragingCTX *) avg_arg->avg_ctx;
	qin = (Queue *) avg_arg->qin;
	qsout = (Queue **) avg_arg->qsout;
	qsout_cnt = avg_arg->qsout_cnt;

#if defined(VERBOSE) || defined(VERBOSE_AVG) || defined(TID)
#if defined(RPI_GPU)
	fprintf(stderr, "[AVG ] Started.\tTID: %li\n", (long int) syscall(224));
#else
	fprintf(stderr, "[AVG ] Started.\n");
#endif
#endif

	while(1) {
		// Wait for input queue not being empty
		pthread_mutex_lock(qin->mut);
		while(qin->empty) {
			// No more input is coming to this queue
			if(qin->exit) {
				pthread_mutex_unlock(qin->mut);
				goto EXIT;
			}
			// Wait for more input coming to this queue
			pthread_cond_wait(qin->notEmpty, qin->mut);
		}

		// Read item from input queue
		iout = (Item *) QUE_remove(qin);
		pthread_mutex_unlock(qin->mut);
		pthread_cond_signal(qin->notFull);

		// Average avg_index items
		avg_index = iout->avg_index;
		fft_size = 1<<iout->log2_fft_size;

#if defined(VERBOSE) || defined(VERBOSE_AVG)
		fprintf(stderr, "[AVG ] Pull item. IND:\t%u\n", avg_index);
#endif

		for(j=0; j<fft_size; ++j) iout->samples[j] /= avg_index;

		for(i=1; i<avg_index; ++i) {
			// Wait for input queue not being empty
			pthread_mutex_lock(qin->mut);
			while(qin->empty) {
				// No more input is coming to this queue
				if(qin->exit) {
					pthread_mutex_unlock(qin->mut);
					goto EXIT;
				}
				// Wait for more input coming to this queue
				pthread_cond_wait(qin->notEmpty, qin->mut);
			}

			// Read item from input queue
			iin = (Item *) QUE_remove(qin);
			pthread_mutex_unlock(qin->mut);
			pthread_cond_signal(qin->notFull);

#if defined(VERBOSE) || defined(VERBOSE_AVG)
			fprintf(stderr, "[AVG ] Pull item. IND:\t%u\n", iin->avg_index);
#endif

			// Assert correct items
			assert(iin->avg_index == avg_index-i);

			for(j=0; j<fft_size; ++j) iout->samples[j] += iin->samples[j] / avg_index;

			// Release input item
			ITE_free(iin);

		}

		// Strategy dependent callback to the monitoring logic
		if(avg_ctx != NULL && avg_ctx->callback != NULL)
			avg_ctx->callback(iout);

		// Single output queue
		if(qsout_cnt == 1) {
			// Wait for output queue not being full
			pthread_mutex_lock(qsout[0]->mut);
			while(qsout[0]->full) {
#if defined(VERBOSE) || defined(VERBOSE_AVG)
				fprintf(stderr, "[AVG ] Output queue 0 full.\n");
#endif
				pthread_cond_wait(qsout[0]->notFull, qsout[0]->mut);
			}
			QUE_insert(qsout[0], iout);
#if defined(VERBOSE) || defined(VERBOSE_AVG)
			fprintf(stderr, "[AVG ] Push item %u to output queue 0.\n", iout->avg_index);
#endif
			pthread_mutex_unlock(qsout[0]->mut);
			pthread_cond_signal(qsout[0]->notEmpty);
		}
		// Multiple output queues
		else if(qsout_cnt > 1) {
			// Put item to output queues
			for(k=0; k<qsout_cnt; ++k) {
				// Wait for output queue not being full
				pthread_mutex_lock(qsout[k]->mut);
				while(qsout[k]->full) {
#if defined(VERBOSE) || defined(VERBOSE_AVG)
					fprintf(stderr, "[AVG ] Output queue %zd full.\n", k);
#endif
					pthread_cond_wait(qsout[k]->notFull, qsout[k]->mut);
				}
				nout = ITE_copy(iout);
				// Write item to output queue
				QUE_insert(qsout[k], nout);
#if defined(VERBOSE) || defined(VERBOSE_AVG)
				fprintf(stderr, "[AVG ] Push item %u to output queue %zd.\n", nout->avg_index, k);
#endif
				pthread_mutex_unlock(qsout[k]->mut);
				pthread_cond_signal(qsout[k]->notEmpty);
			}
			// Release item
			ITE_free(iout);
		}

	}

	EXIT:

	// Signal that we are done and no further items will appear in the queues
	for(k=0; k<qsout_cnt; ++k) {
		pthread_mutex_lock(qsout[k]->mut);
		qsout[k]->exit = 1;
		pthread_cond_signal(qsout[k]->notEmpty);
		pthread_cond_signal(qsout[k]->notFull);
		pthread_mutex_unlock(qsout[k]->mut);
	}

#if defined(VERBOSE) || defined(VERBOSE_AVG)
	fprintf(stderr, "[AVG ] Terminated.\n");
#endif

	pthread_exit(NULL);

}

static void* dumping(void *args) {

#if defined(VERBOSE) || defined(VERBOSE_DUMP)
	fprintf(stderr, "[DUMP] Started.\n");
#endif

	int i, t;

	uint32_t freq;
	uint32_t center_freq;
	uint32_t fft_size, prev_reduced_fft_size = 0, reduced_fft_size;

	float    freq_res;
	float    *samples;

	DumpingARG   *dump_arg;

	Item  *iin;
	Queue *qin;

	// Parse arguments
	dump_arg = (DumpingARG *) args;
	qin = (Queue *) dump_arg->qin;

	// Dump full plain-text data to stdout
	while(1) {
		// Wait for input queue not being empty
		pthread_mutex_lock(qin->mut);
		while(qin->empty) {
			// No more input is coming to this queue
			if(qin->exit) {
				pthread_mutex_unlock(qin->mut);
				goto EXIT;
			}
			// Wait for more input coming to this queue
			pthread_cond_wait(qin->notEmpty, qin->mut);
		}

		// Read item from input queue
		iin = (Item *) QUE_remove(qin);
#if defined(VERBOSE) || defined(VERBOSE_DUMP)
		fprintf(stderr, "[DUMP] Read from Queue.\n");
#endif
		pthread_mutex_unlock(qin->mut);
		pthread_cond_signal(qin->notFull);

		center_freq = iin->Fc;
		//reduced_fft_size = iin->reduced_fft_size;
		freq_res = iin->freq_res;
		samples = iin->samples;

		// Get item's characteristics
		fft_size = 1 << iin->log2_fft_size;
		reduced_fft_size = (1-iin->freq_overlap)*(fft_size+1);
		freq_res = ((float) iin->samp_rate) / fft_size;
		// Check for changing FFT sizes
		if(reduced_fft_size != prev_reduced_fft_size) {
			prev_reduced_fft_size = reduced_fft_size;
		}

		// Write item to file
#if defined(VERBOSE) || defined(VERBOSE_DUMP)
		fprintf(stderr, "[DUMP] Write to stdout: %zd reduced_ffts .\n", reduced_fft_size);
#endif
		t = reduced_fft_size / 2;

		for(i=0; i<reduced_fft_size; ++i) {
			freq = center_freq - (t-i)*freq_res;
			fprintf(stdout, "%u,%u,%u,%.1f\n", iin->Ts_sec, iin->Ts_usec, freq, samples[i]);
		}
		fflush(stdout);

		// Release item
		free(samples);
		free(iin);

	}

	EXIT:

#if defined(VERBOSE) || defined(VERBOSE_DUMP)
	fprintf(stderr, "[DUMP] Terminated.\n");
#endif

	pthread_exit(NULL);
}
