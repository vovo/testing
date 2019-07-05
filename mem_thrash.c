#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>

void fallocate_file(const char *filename, off_t size)
{
	struct stat st;
	int fd;

	if (!stat(filename, &st) && st.st_size >= size)
		return;

	fd = open(filename, O_WRONLY | O_CREAT, 0600);
	if (fd < 0) {
		perror("create file");
		exit(1);
	}
	if (posix_fallocate(fd, 0, size)) {
		perror("fallocate");
		exit(1);
	}
	close(fd);
}

long *alloc_anon(long size)
{
	long *start = malloc(size);
	memset(start, 1, size);
	return start;
}

long pswpout()
{
	long num;
	FILE* fp = popen("awk '/pswpout/{print $2}' /proc/vmstat", "r");
	fscanf(fp, "%ld", &num);
	fclose(fp);
	return num;
}

long pgpgin()
{
	long num;
	FILE* fp = popen("awk '/pgpgin/{print $2}' /proc/vmstat", "r");
	fscanf(fp, "%ld", &num);
	fclose(fp);
	return num;
}

void meminfo(long* ret)
{
	FILE* fp = popen("awk '/[Aa]ctive\\(/{print $2}' /proc/meminfo", "r");
	fscanf(fp, "%ld%ld%ld%ld", &ret[0], &ret[1], &ret[2], &ret[3]);
	fclose(fp);
}


long access_file(const char *filename, long size_exec, long size_reg,
		 long rounds, int index, int procs, long *memdata,
		 double *duration)
{
	int fd, i;
	volatile char *start1, *start2;
	const int page_size = getpagesize();
	long sum = 0;

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		perror("open");
		exit(1);
	}

	/*
	 * Some applications, e.g. chrome, use a lot of executable file
	 * pages, map some of the pages with PROT_EXEC flag to simulate
	 * the behavior.
	 */
	start1 = mmap(NULL, size_exec, PROT_READ | PROT_EXEC, MAP_SHARED,
		      fd, 0);
	if (start1 == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	start2 = mmap(NULL, size_reg, PROT_READ, MAP_SHARED, fd, size_exec);
	if (start2 == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	for (i = 0; i < rounds; ++i) {
		struct timeval before, after;
		volatile char *ptr1 = start1, *ptr2 = start2;
		int j;

		gettimeofday(&before, NULL);
		if (size_exec > size_reg) {
			j = index * size_exec / procs;
			for (; j < ((index + 1) * size_exec / procs); j += page_size) {
				sum += ptr1[j] + ptr2[j * size_reg / size_exec ];
			}
		} else {
			j = index * size_reg / procs;
			for (; j < ((index + 1) * size_reg / procs); j += page_size) {
				sum += ptr2[j] + ptr1[j * size_exec / size_reg ];
			}
		}

		gettimeofday(&after, NULL);
		duration[i * procs + index] =
		    (after.tv_sec - before.tv_sec) +
		    (after.tv_usec - before.tv_usec) / 1000000.0;
		if (index == 0) {
			meminfo(memdata + 4 * i);
		}
	}
	return sum;
}

void* shared_data(long size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

int main(int argc, char *argv[])
{
	const long MB = 1024 * 1024;
	long anon_mb, file_exec_mb, file_reg_mb, file_rounds, procs;
	const char filename[] = "large";
	long *ret1;
	int i;
	long pswpout_begin, pswpout_end;
	long pgin_begin, pgin_end;
	pid_t *pids;
	long *results;
	long *memdata;
	double *duration;
	long memdata0[4];

	if (argc != 6) {
		printf("usage: thrash ANON_MB FILE_EXEC_MB FILE_REG_MB FILE_ROUNDS PROCS\n");
		exit(0);
	}
	anon_mb = atoi(argv[1]);
	file_exec_mb = atoi(argv[2]);
	file_reg_mb = atoi(argv[3]);
	file_rounds = atoi(argv[4]);
	procs = atoi(argv[5]);

	pids = malloc(sizeof(pid_t) * procs);
	results = shared_data(sizeof(long) * procs);
	memdata = shared_data(sizeof(long) * file_rounds * 4);
	duration = shared_data(sizeof(double) * file_rounds * procs);

	fallocate_file(filename, (file_exec_mb + file_reg_mb) * MB);
	printf("Allocate %ld MB anonymous pages\n", anon_mb);
	ret1 = alloc_anon(anon_mb * MB);
	meminfo(memdata0);
	printf("active_anon: %ld, inactive_anon: %ld, active_file: %ld, inactive_file: %ld (kB)\n",
	       memdata0[0], memdata0[1], memdata0[2], memdata0[3]);
	pswpout_begin = pswpout();
	pgin_begin = pgpgin();
	printf("pswpout: %ld, pgpgin: %ld\n", pswpout_begin, pgin_begin);
	printf("Access %ld MB executable file pages\n", file_exec_mb);
	printf("Access %ld MB regular file pages\n", file_reg_mb);

	for (i = 0; i < procs; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			results[i] = access_file(filename, file_exec_mb * MB,
						 file_reg_mb * MB, file_rounds,
						 i, procs, memdata, duration);
			return 0;
		} else {
			pids[i] = pid;
		}
	}
	for (i = 0; i < procs; i++) {
		waitpid(pids[i], NULL, 0);
	}

	// print collected data
	for (i = 0; i < file_rounds; ++i) {
		long *memdata1 = memdata + i * 4;
		int j;

		printf("File access time, round %d: ", i);
		for (j = 0; j < procs; ++j) {
			printf("%.3f, ", duration[i * procs + j]);
		}
		printf("(sec)\n");
		printf("active_anon: %ld, inactive_anon: %ld, active_file: %ld, inactive_file: %ld (kB)\n",
		       memdata1[0], memdata1[1], memdata1[2], memdata1[3]);
	}
	pswpout_end = pswpout();
	pgin_end = pgpgin();
	printf("pswpout: %ld (+ %ld), pgpgin: %ld (+ %ld)\n", pswpout_end,
	       pswpout_end - pswpout_begin, pgin_end, pgin_end - pgin_begin);
	return 0;
}
