#include <stdlib.h>
#include <math.h>
#include "config.h"
#include "chieftain.h"
#include "valhalla.h"

//verifica se a cadeira 'pos' é segura para o tipo 'is_berserker'
static int is_seat_safe(chieftain_t *self, int pos, int is_berserker) {
    int N = config.table_size;
    
    //o vão da mesa (entre cadeira 0 e N-1) atua como barreira segura.
    //a cadeira 0 não conflita com a N-1
    
    if (pos > 0) { //esquerda
        if (self->seat_state[pos - 1] != -1 && self->seat_state[pos - 1] != is_berserker)
            return 0;
    }
    if (pos < N - 1) { //direita
        if (self->seat_state[pos + 1] != -1 && self->seat_state[pos + 1] != is_berserker)
            return 0;
    }
    return 1;
}

//testa se atribuir +1 prece ao deus quebra as regras de tolerância
static int can_assign_god(chieftain_t *self, god_t candidate) {
    int temp[NUMBER_OF_GODS];
    int total_normal = 0;

    for (int i = 0; i < NUMBER_OF_GODS; i++) {
        temp[i] = self->assigned_prayers[i];
        if (i == candidate) temp[i]++;
        if (!valhalla_is_super(i)) total_normal += temp[i];
    }

    //validação do deus (c/ arredondamento para cima)
    for (int god = 0; god < NUMBER_OF_GODS; god++) {
        int count = temp[god];
        if (!valhalla_is_super(god)) {
            int rival = valhalla_get_rival(god);
            int rival_count = temp[rival];
            int max_allowed = (int) ceil(rival_count * (1.0 + RIVAL_TOLERANCE_RATE));
            if (max_allowed < 1) max_allowed = 1;
            int min_allowed = (int) floor(rival_count * (1.0 - RIVAL_TOLERANCE_RATE));
            
            if (count < min_allowed || count > max_allowed) return 0;
        } else {
            int max_allowed = (int) ceil(total_normal * (1.0 + SUPER_GOD_TOLERANCE_RATE));
            if (count > max_allowed) return 0;
        }
    }
    return 1;
}


void chieftain_init(chieftain_t *self, valhalla_t *valhalla)
{
    self->valhalla = valhalla;

    pthread_mutex_init(&self->mutex, NULL);
    pthread_cond_init(&self->cond_banquet, NULL);
    pthread_cond_init(&self->cond_prayers, NULL);

    int N = config.table_size;
    self->seat_state = (int *) malloc(sizeof(int) * N);
    self->plate_state = (int *) malloc(sizeof(int) * N);
    self->viking_plates = (int *) malloc(sizeof(int) * N * 2);

    for (int i = 0; i < N; i++) {
        self->seat_state[i] = -1;
        self->plate_state[i] = 0;
    }

    self->finished_eating_count = 0;
    for (int i = 0; i < NUMBER_OF_GODS; i++)
        self->assigned_prayers[i] = 0;
    
    plog("[chieftain] Initialized\n");
}

int chieftain_acquire_seat_plates(chieftain_t *self, int berserker)
{
    pthread_mutex_lock(&self->mutex);
    int N = config.table_size;
    int acquired_seat = -1;

    while (acquired_seat == -1) {
        for (int i = 0; i < N; i++) {
            if (self->seat_state[i] == -1) { // caso de cadeira vazia
                if (is_seat_safe(self, i, berserker)) {
                    // pratos alinhados a cadeira i, a esquerda e a direita 
                    int p1 = i;
                    int p2 = (i - 1 + N) % N;
                    int p3 = (i + 1) % N;

                    int free_plates[3];
                    int count = 0;
                    if (self->plate_state[p1] == 0) free_plates[count++] = p1;
                    if (self->plate_state[p2] == 0) free_plates[count++] = p2;
                    if (self->plate_state[p3] == 0) free_plates[count++] = p3;

                    if (count >= 2) { //encontrou cadeira válida, pegaR 2 pratos
                        acquired_seat = i;
                        self->seat_state[i] = berserker;
                        
                        int gp1 = free_plates[0];
                        int gp2 = free_plates[1];
                        self->plate_state[gp1] = 1;
                        self->plate_state[gp2] = 1;
                        
                        // guarda quais pratos essa cadeira pegou
                        self->viking_plates[i * 2]     = gp1;
                        self->viking_plates[i * 2 + 1] = gp2;
                        break;
                    }
                }
            }
        }

        if (acquired_seat == -1) {
            pthread_cond_wait(&self->cond_banquet, &self->mutex);
        }
    }

    pthread_mutex_unlock(&self->mutex);
    return acquired_seat;
}

void chieftain_release_seat_plates(chieftain_t *self, int pos)
{
    pthread_mutex_lock(&self->mutex);

    self->seat_state[pos] = -1;
    int p1 = self->viking_plates[pos * 2];
    int p2 = self->viking_plates[pos * 2 + 1];
    self->plate_state[p1] = 0;
    self->plate_state[p2] = 0;

    self->finished_eating_count++;

    //libera a mesa para os que estão na fila
    pthread_cond_broadcast(&self->cond_banquet);

    //se todos os vikings normais comeram, acorda a fila do open bar p/ rezar
    if (self->finished_eating_count == config.horde_size) {
        pthread_cond_broadcast(&self->cond_prayers);
    }

    pthread_mutex_unlock(&self->mutex);
}

god_t chieftain_get_god(chieftain_t *self)
{
    pthread_mutex_lock(&self->mutex);

    //ninguém passa daqui enquanto o banquete não acabar
    while (self->finished_eating_count < config.horde_size) {
        pthread_cond_wait(&self->cond_prayers, &self->mutex);
    }

    god_t chosen_god = BALDR;
    god_t shuffled[NUMBER_OF_GODS] = {BALDR, LOKI, VALI, HODER, FRIGG, JORD, ODIN, THOR};

    //embaralha o array de deuses aleatoriamente
    for (int i = NUMBER_OF_GODS - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        god_t temp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = temp;
    }

    //varre o array embaralhado e escolhe o primeiro deus que não quebra as regras
    for (int k = 0; k < NUMBER_OF_GODS; k++) {
        god_t candidate = shuffled[k];
        if (can_assign_god(self, candidate)) {
            chosen_god = candidate;
            self->assigned_prayers[chosen_god]++;
            break;
        }
    }

    pthread_mutex_unlock(&self->mutex);
    return chosen_god;
}

void chieftain_finalize(chieftain_t *self)
{
    free(self->seat_state);
    free(self->plate_state);
    free(self->viking_plates);

    pthread_cond_destroy(&self->cond_banquet);
    pthread_cond_destroy(&self->cond_prayers);
    pthread_mutex_destroy(&self->mutex);

    plog("[chieftain] Finalized\n");
}
