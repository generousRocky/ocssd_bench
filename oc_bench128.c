#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <liblightnvm.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "profile.h"

#define IS_ISOLATED 0

// same as the number of LUN in VBLK
#define NR_BLKS_IN_VBLK 4 // must not be beyond (NR_PUNITS/2)

#define NR_W_THREADS 128
#define NR_R_THREADS 0

#define NBYTES_TO_WRITE 68719476736 // 64GB
#define NBYTES_TO_READ 68719476736 // 64GB

#define NVM_DEV_PATH "/dev/nvme0n1"

// lun1개에 읽기 쓰기 실험을 하고 싶은 경우 NR_PUNITS 2로 해야 각각 1, 1로 실험 할 수 있음.
#define NR_PUNITS 128
#define NR_BLOCKS 1020

#define NR_VBLKS_W (size_t)(1020 * ((NR_PUNITS / NR_BLKS_IN_VBLK) / (double)2))
#define NR_VBLKS_R (size_t)(1020 * ((NR_PUNITS / NR_BLKS_IN_VBLK) / (double)2))

#define DEBUG_ENABLE 1
#define debug_print(fmt, ...) \
	do { if (DEBUG_ENABLE) fprintf(stderr, "%s::%d::%s(): " fmt, __FILE__, \
			__LINE__, __func__, ##__VA_ARGS__); } while (0)

enum{
	FREE,
	RESERVED,
	BAD,
};

pthread_mutex_t  mutex_w = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  mutex_r = PTHREAD_MUTEX_INITIALIZER;

static char nvm_dev_path[] = NVM_DEV_PATH;
static struct nvm_dev *dev;
static const struct nvm_geo *geo;

struct nvm_buf_set *bufs = NULL;
struct nvm_addr punits_[NR_PUNITS];

struct nvm_vblk* vblks_w[NR_VBLKS_W];
struct nvm_vblk* vblks_r[NR_VBLKS_R];

int vblks_state_r[NR_VBLKS_R];
int vblks_state_w[NR_VBLKS_W];

int curs_w=-1;
int curs_r=-1;

size_t NBYTES_IO_W = (NR_W_THREADS == 0) ? 0 : (size_t)(NBYTES_TO_WRITE/NR_W_THREADS); // per a thread
size_t NBYTES_IO_R = (NR_R_THREADS == 0) ? 0 : (size_t)(NBYTES_TO_WRITE/NR_R_THREADS); // per a thread
size_t NBYTES_VBLK = (size_t)NR_BLKS_IN_VBLK*1024*1024*16;


void free_all(){
	for(size_t i=0; i<NR_VBLKS_W; i++){
		nvm_vblk_free(vblks_w[i]);
	}
	
	for(size_t i=0; i<NR_VBLKS_R; i++){
		nvm_vblk_free(vblks_r[i]);
	}
  
	return;
}

struct nvm_vblk* get_vblk_for_read(void){
	pthread_mutex_lock(&mutex_r);
	
  for (size_t i = 0; i < NR_VBLKS_R; i++) {
		curs_r++;
	  curs_r = (curs_r % NR_VBLKS_R);
	
		switch (vblks_state_r[curs_r]) {
			case RESERVED:
				pthread_mutex_unlock(&mutex_r);
				return vblks_r[curs_r];

			case FREE:
			case BAD:
				debug_print("FREE or BAD - vblk: %d\n", curs_r);
				break;
		}

		debug_print("out of switch\n");
	}

	debug_print("No available vblk for READ\n");
	pthread_mutex_unlock(&mutex_r);
  return NULL;
}

struct nvm_vblk* get_vblk_for_write(void){
	pthread_mutex_lock(&mutex_w);

	for (size_t i = 0; i < NR_VBLKS_W; i++) {
		//debug_print("[WRITE] get_vblk_for_write - iterate: %zu, curs_w: %d\n", i, curs_w);

		curs_w++;
		curs_w = curs_w % (int)NR_VBLKS_W;

		switch (vblks_state_w[curs_w]) {
			case FREE:
				if( nvm_vblk_erase(vblks_w[curs_w]) < 0) {
					vblks_state_w[curs_w] = BAD;

					debug_print("WARNING: fail to erase vblk: %d\n", curs_w);
					break;
				}

				vblks_state_w[curs_w] = RESERVED;
				pthread_mutex_unlock(&mutex_w);
				//debug_print("[WRITE] get_vblk: %d\n", curs_w);
				return vblks_w[curs_w];

			case RESERVED:
			case BAD:
				break;
		}
	}

	debug_print("No available vblk\n");
	pthread_mutex_unlock(&mutex_w);
  return NULL;
}

void nvm_init(){

  size_t nbytes=0;

	/* open nvme device */
	dev = nvm_dev_open(nvm_dev_path);
	if (!dev) {
		perror("nvm_dev_open");
		return exit(-1);
	}

	geo = nvm_dev_get_geo(dev);
	
	debug_print("Allocating nvm_buf_set\n");
	bufs = nvm_buf_set_alloc(dev, NBYTES_VBLK, 0);
	if (!bufs) {
		debug_print("FAILED: Allocating nvm_buf_set\n");
		exit(-1);
	}

	debug_print("nvm_buf_set_fill\n");
	nvm_buf_set_fill(bufs);

	// Construct punit addrs
	for (size_t i = 0; i < 128; ++i){
		struct nvm_addr addr;

		addr.ppa = 0;
		addr.g.ch = i % geo->nchannels;
		addr.g.lun = (i / geo->nchannels) % geo->nluns;

		punits_[i] = addr;
	}

  // Initialize and allocate vblks with defaults (Free)
	size_t vblk_w_idx = 0;
	size_t vblk_r_idx = 0;
	
	size_t nr_iterate_to_read = 0; // up to NBYTES_TO_READ / NBYTES_VBLK;
	debug_print("up to nr_iterate_to_read: %zu\n", NBYTES_TO_READ / NBYTES_VBLK);


	for(size_t blk_idx =0; blk_idx< NR_BLOCKS; blk_idx++){
		//debug_print("blk_idx: %zu\n", blk_idx);
		for(size_t i=0; i<NR_PUNITS/NR_BLKS_IN_VBLK; i++){ // i < 128/32 // i = 0, 1, 2, 3
			

			struct nvm_vblk *blk;
			struct nvm_addr addrs[NR_BLKS_IN_VBLK];
		
			for(size_t j=0; j<NR_BLKS_IN_VBLK; j++){
				addrs[j] = punits_[NR_BLKS_IN_VBLK*i+j];
				addrs[j].g.blk = blk_idx;
			}

			blk = nvm_vblk_alloc(dev, addrs, NR_BLKS_IN_VBLK);
			if (!blk) {
				perror("FAILED: nvm_vblk_alloc_line");
				exit(-1);
			}
			
#if IS_ISOLATED	
			if(i < (NR_PUNITS/NR_BLKS_IN_VBLK)/2){ // e.g. (128/32)/2 = 2 // i == 0, 1
#else
			if(vblk_w_idx < NR_VBLKS_W){ // e.g. (128/32)/2 = 2 // i == 0, 1
#endif
				//vblks reserved for writes
				//debug_print("vblk_w_idx: %zu is initialized\n", vblk_w_idx);
				vblks_w[vblk_w_idx] = blk;
				vblks_state_w[vblk_w_idx] = FREE;
				vblk_w_idx++;
			}
			else{
				if(NR_R_THREADS == 0 ){
					continue;
				}
				// half of vblks are reserved of READ

				if(nr_iterate_to_read < (NBYTES_TO_READ/NBYTES_VBLK) ){
					if (nvm_vblk_erase(blk) < 0) {
						vblks_state_r[vblk_r_idx] = BAD;
					}
					else{
						if (nvm_vblk_write(blk, bufs->write, NBYTES_VBLK) < 0) {
							debug_print("FAILED: nvm_vblk_write\n");
							vblks_state_r[vblk_r_idx] = BAD;
							continue;
						}

						//debug_print("vblk_r_idx: %zu is wriiten and reserved for READ\n", vblk_r_idx);
						vblks_state_r[vblk_r_idx] = RESERVED;
						nr_iterate_to_read++;	
					}
				}
				else{
					vblks_state_r[vblk_r_idx] = FREE;
				}

				vblks_r[vblk_r_idx] = blk;
				vblk_r_idx++;
			}
		}
	}
}

void *t_writer(void *data)
{
	struct nvm_vblk* vblk; 

	size_t* size = (size_t*)data;
	int nr_iterate = ((*size)/NBYTES_VBLK);
	
	debug_print("[WIRTE] size: %zu\n", *size);
	debug_print("[WIRTE] NBYTES_VBLK: %zu\n", NBYTES_VBLK);
	
	if((*size)%NBYTES_VBLK > 0){
		nr_iterate++;
	}

	debug_print("[WIRTE] nr_iterate: %d\n", nr_iterate);
	for(size_t i=0; i< nr_iterate; i++){
		//debug_print("[WIRTE] %zu th iterate\n", i);

		//debug_print("[WIRTE] get vblk for write\n");
    vblk = get_vblk_for_write();
    
		if(vblk == NULL){
      debug_print("[WIRTE] FAILED: Allocating vblk\n");
      goto out;
    }
		
		//debug_print("[WIRTE] nvm_vblk_write\n");
    if (nvm_vblk_write(vblk, bufs->write, NBYTES_VBLK) < 0) {
      debug_print("[WIRTE] FAILED: nvm_vblk_write\n");
			goto out;
    }
	}
	
  debug_print("[WIRTE] nvm_vblk_write: done\n");
  return 0;

	debug_print("[write] FAILED: out\n");
out:
  nvm_buf_set_free(bufs);
  free_all();
  exit(-1);
}

// 멀티 스레드랑 읽는 회수랑 cur랑 지금 안맞는 것 같음.
void *t_reader(void *data)
{
  struct nvm_vblk* vblk; 
	
	size_t* size = (size_t*)data;
	int nr_iterate = ((*size)/NBYTES_VBLK);
	
	debug_print("[READ] size: %zu\n", *size);
	debug_print("[READ] NBYTES_VBLK: %zu\n", NBYTES_VBLK);
	
	if((*size)%NBYTES_VBLK > 0){
		nr_iterate++;
	}
	
	debug_print("[READ] nr_iterate: %d\n", nr_iterate);
	for(size_t i=0; i< nr_iterate; i++){
		//debug_print("[READ] %zu th iterate\n", i);

    //debug_print("[READ] Allocating vblk\n");
    vblk = get_vblk_for_read();
    if(vblk == NULL){
      debug_print("[READ] FAILED: Allocating vblk\n");
      goto out;
    }
		
		//debug_print("[READ] nvm_vblk_read\n");
    if (nvm_vblk_read(vblk, bufs->read, NBYTES_VBLK) < 0) {
      debug_print("[READ] FAILED: nvm_vblk_read\n");
      goto out;
    }
	}
	
  debug_print("[READ] nvm_vblk_read: done\n");
  return 0;

	debug_print("[write] FAILED: out\n");
out:
  nvm_buf_set_free(bufs);
  free_all();
  exit(-1);

}

void *io_manager(void *data){

  struct timespec local_time[2];
  clock_gettime(CLOCK_MONOTONIC, &local_time[0]);
  unsigned long long delay_time;
  // 프로파일링 시작
	
	pthread_t w_thread[NR_W_THREADS];
	pthread_t r_thread[NR_R_THREADS];

	size_t args_w_size[NR_W_THREADS];
	size_t args_r_size[NR_R_THREADS];

	int thr_id;
	int status;
	int rw = *((int*)data);
	

	if(rw == 0){ // writer
		for(int i=0; i<NR_W_THREADS; i++){
			args_w_size[i] = NBYTES_IO_W;
			thr_id = pthread_create(&w_thread[i], NULL, t_writer, &args_w_size[i]);
			if (thr_id < 0){
				perror("thread create error : ");
				exit(0);
			}
		}	
	}
	
	if(rw == 1){ // reader
		for(int i=0; i<NR_R_THREADS; i++){
			args_r_size[i] = NBYTES_IO_R;
			thr_id = pthread_create(&r_thread[i], NULL, t_reader, &args_r_size[i]);
			if (thr_id < 0){
				perror("thread create error : ");
				exit(0);
			}
		}	
	}

	if(rw == 0){ // writer
		for(int i=0; i<NR_W_THREADS; i++){
			pthread_join(w_thread[i], (void **)&status);
		}	
	}
	
	if(rw == 1){ // reader
		for(int i=0; i<NR_R_THREADS; i++){
			pthread_join(r_thread[i], (void **)&status);
		}	
	}

	// 프로파일링 끝
  clock_gettime(CLOCK_MONOTONIC, &local_time[1]);
  calclock(local_time, &total_time, &total_count, &delay_time );

	if(rw == 0){
		printf("[WRITE], elapsed time: %llu,  total_time: %llu, total_count: %llu\n", delay_time, total_time, total_count);
		printf("[WRITE] MiB/s: %LF\n", ((long double)NBYTES_TO_WRITE/(1024*1024))/(total_time/((double)1000000000)));
	}
	else{
		printf("[READ], elapsed time: %llu,  total_time: %llu, total_count: %llu\n", delay_time, total_time, total_count);
		printf("[READ] MiB/s: %LF\n", ((long double)NBYTES_TO_READ/(1024*1024))/(total_time/((double)1000000000)));
	}
}


int main()
{
	pthread_t m_thread[2];

	int thr_id;
	int status;

	int flag_writer=0;
	int flag_reader=1;

	if( (NBYTES_IO_W % NBYTES_VBLK) != 0 ){
		printf("Write Size: miss aligned\n");
		exit(0);
	}
	
	if( (NBYTES_IO_R % NBYTES_VBLK) != 0 ){
		printf("Read Size: miss aligned\n");
		exit(0);
	}
	
	nvm_init();

	if(NR_W_THREADS > 0){
		thr_id = pthread_create(&m_thread[0], NULL, io_manager, &flag_writer);
		if (thr_id < 0){
			perror("thread create error : ");
			exit(0);
		}
	}

	if(NR_R_THREADS > 0){
		thr_id = pthread_create(&m_thread[1], NULL, io_manager, &flag_reader);
		if (thr_id < 0){
			perror("thread create error : ");
			exit(0);
		}
	}

	if(NR_W_THREADS > 0){
		pthread_join(m_thread[0], (void **)&status);
	}

	if(NR_R_THREADS > 0){
		pthread_join(m_thread[1], (void **)&status);
	}

	// free
	free_all();
  nvm_buf_set_free(bufs);
	
	/* close device */
	nvm_dev_close(dev);
	
	return 0;
}


