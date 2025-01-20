#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <pthread.h>
#include <signal.h>

//kolorowanie wierszy
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"

#define NUM_PRODUCTS 11  // Liczba podajnikow
#define MAX_AMOUNT 20    // Maksymalna liczba sztuk dla kazdego produktu na podajniku
#define MAX_SLEEP 2      // Maksymalny czas opoznienia w sekundach przed dodaniem nowego produktu
#define CENNIK_SHM 55    // Klucz do pamieci z podajnikami (dane do odczytu dla kasjer.c)
#define SEM_PRICES 999   // klucz do semafora do wylaczenia pamieci dzielonej z cenami po odczycie przez kasjer.c
#define PID_SHM 555      //pamiec dzielona na pidy procesow dla kierownik.c

// Funkcja do operacji sem_wait (czekanie na semafor)
void sem_wait_op(int semid) {
    struct sembuf sops;
    sops.sem_num = 0;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = -1;    // Operacja dekrementacji (czekanie na semafor)
    sops.sem_flg = 0;    // Flagi (brak)

    // Czekamy na semafor (jesli jego wartosc to 0)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_wait)");
        exit(1);
    }
}

// Funkcja do operacji sem_post (zwalnianie semafora)
void sem_post_op(int semid) {
    struct sembuf sops;
    sops.sem_num = 0;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = 1;     // Operacja inkrementacji (zwalnianie semafora)
    sops.sem_flg = 0;    // Flagi (brak)

    // Zwalniamy semafor (zwiekszamy jego wartosc)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_post)");
        exit(1);
    }
}

// Funkcja do operacji sem_wait (czekanie na semafor) DLA GRUPY SEMAFOROW
void grp_sem_wait_op(int semid,int sem_num) {
    struct sembuf sops;
    sops.sem_num = sem_num;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = -1;          // Operacja dekrementacji (czekanie na semafor)
    sops.sem_flg = 0;          // Flagi (brak)

    // Czekamy na semafor (jesli jego wartosc to 0)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_wait)");
        exit(1);
    }
}
// Funkcja do operacji sem_post (zwalnianie semafora) DLA GRUPY SEMAFOROW
void grp_sem_post_op(int semid, int sem_num) {
    struct sembuf sops;
    sops.sem_num = sem_num;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = 1;           // Operacja inkrementacji (zwalnianie semafora)
    sops.sem_flg = 0;          // Flagi (brak)

    // Zwalniamy semafor (zwiekszamy jego wartosc)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_post)");
        exit(1);
    }
}

// Struktura dla podajnikow
typedef struct {
    char fifo_path[64];   // Sciezka do FIFO dla danego podajnika
    int initial_stock;    // Poczatkowa liczba dostepnych produktow
    float price;          // Cena produktu
} Dispenser;

// Struktura dla danych piekarza (liczenie wyprodukowanych towarow)
typedef struct {
    int product_id[NUM_PRODUCTS];
} Baker;

Baker baker;                         // Struktura piekarza do liczenia wyprod. prod.
Dispenser dispensers[NUM_PRODUCTS];  // Podajniki dla produktow
pthread_t *bakers_threads;           // Tablica watkow dla piekarzy
int num_bakers = 1;                  // Liczba piekarzy, piekarz jest jeden
int stop_program = 0;                // Flaga do zatrzymania programu (po nacisnieciu Ctrl+C)

//wskaznik do pamieci dzielonej dla danych produktow i cen (dla kasjera)
void *shm_ptr = 0;
//id do pamieci dzielonej dla danych produktow i cen (dla kasjera)
int shm_id = 0;

//flaga dla sygnalu2 (inwenteryzacja)
int flg_inwent = 0;

// Struktura pamieci z pidami procesow
typedef struct {
    pid_t piekarz;
    pid_t kasjer;
    pid_t klient;
} PidsData;

//PAMIEC DLA PIDOW
//wskaznik do pamieci dzielonej
void *pids_shm_ptr;
//id do pamieci dzielonej
int pids_shm_id;
//Struktura do odczytu z pamieci PIDow
PidsData *sharedPids;
//------

//Tworzenie grupy semaforow dla podajnikow (kontrola dostepu do podajnikow miedzy klient.c a piekarz.c)
int dispensers_sem = 100;
int dispensers_sem_id;

// Funkcja inicjalizująca potoki (FIFO) dla podajnikow oraz wstawiajaca poczatkowe liczby produktow
int init_dispensers() {
    for (int i = 0; i < NUM_PRODUCTS; i++) {
        snprintf(dispensers[i].fifo_path, sizeof(dispensers[i].fifo_path), "./podajnik_%d", i + 1);
        dispensers[i].initial_stock = 10;  // Poczatkowa liczba produktow
        dispensers[i].price = (rand() % 1000 + 1) / 100.0;  // Losowa cena od 0.01 do 10.00

        // Usun istniejacy plik i utworz nowy FIFO
        unlink(dispensers[i].fifo_path);  // Usun FIFO, jesli istnieje
        //Tworzenie fifo podajnikow
        if (mkfifo(dispensers[i].fifo_path, 0600) == -1) {
            perror("mkfifo");
            exit(1);
        }

        // Otwieranie FIFO w trybie zapisu/odczytu
        int fifo_fd = open(dispensers[i].fifo_path, O_RDWR);
        if (fifo_fd == -1) {
            perror("open FIFO for writing/reading");
            exit(1);
        }

        // Wstawiamy poczatkowa liczbe produktow do FIFO
        if (write(fifo_fd, &dispensers[i].initial_stock, sizeof(dispensers[i].initial_stock)) == -1) {
            perror("write to FIFO");
            close(fifo_fd);
            exit(1);
        }
        //Zapisanie ilosci do danych piekarza (liczenie wyprodukowanych)
        baker.product_id[i] += dispensers[i].initial_stock;

        printf(GREEN"Piekarz: Podajnik %d z początkową liczba produktow: %d, cena: %.2f\n"RESET, 
                i + 1, dispensers[i].initial_stock, dispensers[i].price);
    }//for

    //Tworzenie pamieci dzielonej dla danych produktow i cen (dla kasjera)
    shm_id = shmget(CENNIK_SHM, sizeof(dispensers), IPC_CREAT | 0600);
    if(shm_id == -1){
        perror("shmget");
        return EXIT_FAILURE;
    }
    //Wskaznik do pamieci
    shm_ptr = shmat(shm_id, NULL, 0);
    if(shm_ptr == (void *) -1){
        perror("shmat");
        return EXIT_FAILURE;
    }

    //Zapisywanie danych do pamieci dzielonej
    memcpy(shm_ptr,dispensers,sizeof(dispensers));
    printf("Zapisano dane produktow do pamieci\n");
    //stworzenie semafora dla kontroli odczytu danych z pamieci
    //po odczycie danych przez kasjer.c ustawiamy semafor i usuwamy pamiec
	int prices_semid = semget(SEM_PRICES,1, IPC_CREAT | 0600);
	if(prices_semid == -1){
		perror("semget failed");
		exit(1);
	}
	//ustawienie semafora na 0
	if(semctl(prices_semid, 0, SETVAL, 0)==-1){
		perror("semctl SETVAL failed");
		exit(1);
	}
	//czekamy na sem_post() od kasjer.c
	sem_wait_op(prices_semid);
    //zwalniamy pamiec dzielona i semafor

    //zwalniamy semafor
	if(semctl(prices_semid,0,IPC_RMID)==-1){
		perror("semctl IPC_RMID failed");
		exit(1);
	}
	//odlaczanie pamieci dzielonej
	if(shmdt(shm_ptr) == -1){
        perror("shmdt");
        return EXIT_FAILURE;
        }
	//usuwanie pamieci dzielonej
	if(shmctl(shm_id, IPC_RMID,NULL) == -1){
        perror("shmctl");
        return EXIT_FAILURE;
        }
	printf("Kasjer odczytal dane - zwolniono pamiec\n");
}

// Funkcja piekarza - dodawanie losowego produktu do podajnika
// Synchronizacja dostepu do podajnikow z klient.c za pomoca semaforow
void *baker_thread(void *arg) {
    srand(time(NULL) + pthread_self());  // Inicjalizacja generatora liczb losowych dla każdego watku

    while (!stop_program) {
        int product_index = rand() % NUM_PRODUCTS;  // Losowy podajnik (produkt)
        int quantity_to_add = rand() % (MAX_AMOUNT / 2) + 1;  // Losowa liczba produktow do dodania 

        //blokowanie semaforem dostepu do podajnika            
        grp_sem_wait_op(dispensers_sem_id,product_index);//indexy semaforow od 0

        // Otwarcie potoku dla danego produktu
        int fifo_fd_read = open(dispensers[product_index].fifo_path, O_RDONLY);			
        if (fifo_fd_read == -1) {
            perror("open FIFO for reading");
            grp_sem_post_op(dispensers_sem_id,product_index);
            continue;
        }

        // Odczytaj aktualną liczbę dostępnych produktów z FIFO
        int available_stock;
        ssize_t bytes_read = read(fifo_fd_read, &available_stock, sizeof(available_stock));
        if (bytes_read <= 0) {
            printf("Piekarz: Podajnik %d - blad odczytu danych, nie mozna dodac produktow.\n", product_index + 1);
            close(fifo_fd_read);
            //odblokowanie semaforem dostepu do podajnika      
            grp_sem_post_op(dispensers_sem_id,product_index);
	        sleep(rand() % MAX_SLEEP);
            continue;
        }
        //jesli liczba w podajniku + wyprodukowana przekracza max_ilosc to nie dodajemy
        //zapisujemy dane do fifo z powrotem bez zmian
        if(available_stock + quantity_to_add >= MAX_AMOUNT){
            int fifo_fd_write = open(dispensers[product_index].fifo_path, O_WRONLY);		
            if(fifo_fd_write == -1){
                perror("open FIFO for writing");
                //odblokowanie semaforem dostepu do podajnika      
                grp_sem_post_op(dispensers_sem_id,product_index);
                continue;
            }

            ssize_t bytes_written = write(fifo_fd_write, &available_stock, sizeof(available_stock));
            if(bytes_written == -1){
                perror("write to FIFO");
                close(fifo_fd_write);
                grp_sem_post_op(dispensers_sem_id,product_index);
                continue;
            }
            printf(YELLOW"Nie mozna dodac do podajnik %d, osiagnieto limit\n"RESET,product_index + 1);
            close(fifo_fd_write);
            close(fifo_fd_read);
            //odblokowanie semaforem dostepu do podajnika      
            grp_sem_post_op(dispensers_sem_id,product_index);
            // Losowe opóźnienie przed kolejnym dodaniem produktu
            sleep(rand() % MAX_SLEEP);
	        continue;
	    }
	    else{
            //zapisanie liczby prod. do struktury
            // zwiekszenie liczby produktow na podajniku
            available_stock += quantity_to_add;
            //aktualizacja liczby produktow na podajniku
            dispensers[product_index].initial_stock = available_stock;
        }

        int fifo_fd_write = open(dispensers[product_index].fifo_path, O_WRONLY);		
        if(fifo_fd_write == -1){
            perror("open FIFO for writing");
            //odblokowanie semaforem dostepu do podajnika
            grp_sem_post_op(dispensers_sem_id,product_index);
            continue;
        }
	    ssize_t bytes_written = write(fifo_fd_write, &available_stock, sizeof(available_stock));
	    if(bytes_written == -1){
            perror("write to FIFO");
            close(fifo_fd_write);
            //odblokowanie semaforem dostepu do podajnika
            grp_sem_post_op(dispensers_sem_id,product_index);
            continue;
	    }
        //zapisanie ilości do danych piekarza (liczenie wyprodukowanych)
        baker.product_id[product_index] += quantity_to_add;

        printf("Piekarz dodal "GREEN"%d"RESET" produktow do podajnika "YELLOW"%d"RESET", nowa ilosc: %d, cena: %.2f\n", 
                quantity_to_add, product_index + 1, available_stock, dispensers[product_index].price);

        close(fifo_fd_write);
	    close(fifo_fd_read);
        //odblokowanie semaforem dostepu do podajnika      
        grp_sem_post_op(dispensers_sem_id,product_index);
        //losowe opoznienie przed kolejnym dodaniem produktu
        sleep(rand() % MAX_SLEEP);
    }
    return NULL;
}

// Obsluga sygnalow:
// SIGINT (Ctrl+C)
// SIGUSR1 - wysyla kierownik.c (Ctrl+C)
// SIGUSR2 - wysyla kierownik.c (Ctrl+Z)
void handle_sigint(int sig) {
    if(sig == SIGINT){
        stop_program = 1;
        printf("\nZatrzymywanie programu... Potoki zostana usuniete.\n");
        //Wyswietlenie listy wytworzonych towarow ogolem przez piekarza (jesli byl sygnal inwenteryzacji)
        if(flg_inwent == 1){    
            printf("\nWyprodukowane towary przez piekarza:\n");
            for(int i = 0; i<NUM_PRODUCTS;i++){
                printf("Produkt "YELLOW"%d"RESET"   wyprodukowana ilosc:"GREEN"%d"RESET"\n",i+1,baker.product_id[i]);
            }
            printf("\nProdukty pozostale na podajnikach:\n");//odczyt z podajnikow
            for(int i = 0; i<NUM_PRODUCTS;i++){
                // otwarcie potoku dla danego produktu
                int fifo_fd_read = open(dispensers[i].fifo_path, O_RDONLY);			
                if (fifo_fd_read == -1) {
                    perror("open FIFO for reading");
                    continue;
                }
                // odczytanie aktualnej liczby dostepnych produktow z FIFO
                int end_stock;
                ssize_t bytes_read = read(fifo_fd_read, &end_stock, sizeof(end_stock));
                if (bytes_read <= 0) {
                    printf("Piekarz: Podajnik %d - blad odczytu danych, nie mozna dodac produktow.\n", i + 1);
                    close(fifo_fd_read);
                    sleep(rand() % MAX_SLEEP);
                    continue;
                }
                close(fifo_fd_read);
                printf("Produkt "YELLOW"%d"RESET"   pozostala ilosc:"GREEN"%d"RESET"\n",i+1,end_stock);
            }
        }
        // Zatrzymanie watkow piekarzy - piekarz jest jeden
        for (int i = 0; i < num_bakers; i++) {
            pthread_cancel(bakers_threads[i]);
            pthread_join(bakers_threads[i], NULL);  // Czekanie na zakonczenie watkow
        }

        // Usuniecie potokow podajnikow
        for (int i = 0; i < NUM_PRODUCTS; i++) {
            unlink(dispensers[i].fifo_path);  // Usuniecie FIFO podajnika
        }

        //Zwalnianie grupy semaforow synchronizacji dostepu do podajnikow
        if(semctl(dispensers_sem_id,0,IPC_RMID)==-1){
            perror("semctl IPC_RMID failed");
            exit(1);
        }
        free(bakers_threads);  // Zwolnienie pamieci
        exit(0);  // Zakonczenie programu
    }
    //Obsluga sygnalu kierownika signal1
    if(sig == SIGUSR1){
        stop_program = 1;
        printf(RED"\n      EWAKUACJA"RESET"\n\n");
        printf(GREEN"Piekarz opuszcza sklep\n"RESET);

        // Usuniecie potokow podajnikow
        for (int i = 0; i < NUM_PRODUCTS; i++) {
            unlink(dispensers[i].fifo_path);  // Usuniecie FIFO podajnika
        }

        //zwalnianie grupy semaforow synchronizacji dostepu do podajnikow
        //przed zwolnieniem zwiekszamy wartosc na semaforach
        for (int i = 0; i < NUM_PRODUCTS; i++) {
            semctl(dispensers_sem_id,i,SETVAL,1);
        }
        semctl(dispensers_sem_id,0,IPC_RMID);
        // Zatrzymanie watkow piekarzy - piekarz jest jeden
        for (int i = 0; i < num_bakers; i++) {
            pthread_cancel(bakers_threads[i]);
            pthread_join(bakers_threads[i], NULL);  // Czekanie na zakonczenie watkow
        }
        free(bakers_threads);  // Zwolnienie pamieci
        exit(0);  // Zakonczenie programu
    }
    //obsluga sygnalu kierownika signal2
    if(sig == SIGUSR2){
        printf(YELLOW"\n      Otrzymano sygnal o inwenteryzacji\n\n"RESET);
        //zmiana tej flagi powoduje dodatkowa funkcje przy zakonczeniu programu SIGINT
        flg_inwent = 1;
    }
}

int main() {
    srand(time(NULL));  // Inicjalizacja generatora liczb losowych
    //--------------------
    // Rejestracja obslugi sygnalow
    struct sigaction sa1, sa2;
    sa1.sa_handler = handle_sigint;
    sa2.sa_handler = handle_sigint;
    sigemptyset(&sa1.sa_mask);
    sigemptyset(&sa2.sa_mask);
    sa1.sa_flags = 0;
    sa2.sa_flags = 0;

    sigaction(SIGUSR1, &sa1, NULL);
    sigaction(SIGUSR2, &sa2, NULL);
    //--------------------
    //Dostep do pamieci dzielonej i zapisanie tam pidu tego procesu
    //Tworzenie pamieci dzielonej dla PIDow procesow
    pids_shm_id = shmget(PID_SHM, sizeof(PidsData), IPC_CREAT | 0600);
    if(pids_shm_id == -1){
	    perror("shmget");
	    return EXIT_FAILURE;
	}
    //wskaznik do pamieci
    pids_shm_ptr = shmat(pids_shm_id, NULL, 0);
    if(pids_shm_ptr == (void *) -1){
	    perror("shmat");
	    return EXIT_FAILURE;
    }
    //Test odczytu danych
    sharedPids = (PidsData *)pids_shm_ptr;  // Odczyt danych z pamieci
    //Zapis pidu do pamieci
    sharedPids->piekarz = getpid();
    //Zapisywanie danych do pamieci dzielonej

	//Odlaczanie pamieci dzielonej
	if(shmdt(pids_shm_ptr) == -1){
        perror("shmdt");
        return EXIT_FAILURE;
    }
//-------------

    //zerowanie danych piekarza przy uruchomieniu (l. wyprodukowanych)
    memset(&baker, 0, sizeof(Baker));

    // Inicjalizacja podajnikow i FIFO
    init_dispensers();

    // Ustawienie obslugi sygnalu SIGINT (Ctrl+C)
    signal(SIGINT, handle_sigint);

    //Tworzenie zbioru NUM_PRODUCTS semaforow dla synch. dostepu do podajnikow (klientow i piekarzy)
    dispensers_sem_id = semget(dispensers_sem,NUM_PRODUCTS,IPC_CREAT | 0600);
    if(dispensers_sem_id == -1){
        perror("semget failed");
        exit(1);
    }
    //domyslna wartosc semaforow 1
    for(int i = 0; i < NUM_PRODUCTS; i++){
	    if(semctl(dispensers_sem_id,i,SETVAL,1)==-1){
            perror("semctl SETVAL failed");
            exit(1);
    	}
    }

    // Tworzenie watkow dla piekarzy - piekarz jest jeden
    bakers_threads = (pthread_t *)malloc(num_bakers * sizeof(pthread_t));
    for (int i = 0; i < num_bakers; i++) {
        pthread_create(&bakers_threads[i], NULL, baker_thread, NULL);
    }

    // Program dziala w tle - piekarz dodaje produkty
    printf(BLUE"Piekarz: Program uruchomiony. Nacisnij Ctrl+C, aby zakonczyc.\n"RESET);

    // Program bedzie dzialal do momentu, gdy nie otrzyma sygnału SIGINT - albo sygnalu zakonczenia od kierownik.c
    while (!stop_program) {
        sleep(1);  // Glowna petla - czeka na sygnal zamkniecia
    }

    // Zwalnianie watku piekarza na zakonczenie
    free(bakers_threads);  // Zwolnienie pamieci
    printf(GREEN"Piekarz: Program zakonczony.\n"RESET);

    return 0;
}
