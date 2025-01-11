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
#include <pthread.h>
#include <signal.h>
#include <sys/sem.h>
#include <errno.h>

//kolorowanie wierszy
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"

#define NUM_PRODUCTS 11  // Liczba podajnikow
#define MAX_SLEEP 2      // Maksymalny czas opoznienia w sekundach przed dodaniem nowego produktu
#define CENNIK_SHM 55    // Klucz do pamieci z podajnikami (odczyt cen)
#define CASHIER_AM 3     // Ilosc max kasjerow
#define SEM_PRICES 999   // klucz do semafora do wylaczenia pamieci dzielonej z cenami po odczycie przez kasjer.c
#define PID_SHM 555      // pamiec dzielona na pidy dla kierownik.c

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
// Struktura danych klienta - odczyt danych od klientow
typedef struct {
    int client_id;                 // ID klienta
    int products_purchased[NUM_PRODUCTS]; // Liczba produktow pobranych z kazdego podajnika
    char fifo_path[64];//sciezka do potoku
    int client_semid;  //id semafora dla klienta
    int semid;         //id semafora klienta (uzyskane)
} ClientData;

// Struktura dla podajnikow - do odczytu z pamiec od piekarz.c
typedef struct {
    char fifo_path[64];   // Sciezka do FIFO dla danego podajnika
    int initial_stock;    // Poczatkowa liczba dostępnych produktow
    float price;          // Cena produktu
} Dispenser;

// Struktura dla danych produktu - odczt pocz. danych z piekarz.c oraz zapis sprzedanych ilosci przez kasjer.c
typedef struct {
    float total_price;          // Ogolna cena produktu danego typu
    int sold_amount;		    // Ogolna ilosc sprzedanych towarow
} Product;

typedef struct {
    char stall_name[20]; //nazwa kasy dla fifo
    int stall_id; //id kasy
    float stall_total_price; //calkowita kwota uzyskana przez kase
    Product product[NUM_PRODUCTS];
} Cashier_stall;

Cashier_stall cashier_stall[CASHIER_AM];           // Inicjalizacja kas - kazda kasa liczy sprzedane towary i zyski oddzielnie
int stop_program = 0;                // Flaga do zatrzymania programu (po nacisnieciu Ctrl+C)
//wskaznik do pamieci dzielonej - odczyt danych z podajnikow piekarz.c
void *shm_ptr = 0;
//id do pamieci dzielonej - odczyt danych z podajnikow piekarz.c
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
//flaga ewakuacji
int flg_ewak = 0;
//Struktura do odczytu z pamieci danych cen od piekarz.c
Dispenser *dispensers;
//Struktura na zapisane dane z pamieci
Dispenser local_dispensers[NUM_PRODUCTS];
pthread_t *cashiers_threads;           // Tablica watkow dla kasjerow

// Obsługa sygnału SIGINT (Ctrl+C)
void handle_sigint(int sig) {
    if(sig == SIGINT){
        stop_program = 1;
        printf("\nZatrzymywanie programu... Zwalnianie kasjerow.\n");
        if(flg_inwent == 1){
            //Wyswietlenie przed zamknieciem stanu kasy
            printf("Stan kas w chwili zamkniecia:\n");
            for(int i = 0; i<CASHIER_AM;i++){
                printf(YELLOW"\nKasa_%d"RESET"\n",i+1);
                for(int j=0; j<NUM_PRODUCTS;j++){
                    printf("Produkt %d   sprzedana ilosc:%d    calkowity koszt:%0.2f \n",j+1,cashier_stall[i].product[j].sold_amount,cashier_stall[i].product[j].total_price);
                    cashier_stall[i].stall_total_price += cashier_stall[i].product[j].total_price;
                }
            printf("Calkowita kwota uzyskana przez "YELLOW"Kasa_%d"RESET" wynosi: "GREEN"%0.2f"RESET"\n",i+1,cashier_stall[i].stall_total_price);
            }
        }
        // Zatrzymanie watkow kasjerow
        for (int i = 0; i < CASHIER_AM; i++) {
            pthread_cancel(cashiers_threads[i]);
            pthread_join(cashiers_threads[i], NULL);  // Czekanie na zakonczenie watkow
        }

        // Usuniecie potokow
        for (int i = 0; i < CASHIER_AM; i++) {
            unlink(cashier_stall[i].stall_name);  // Usuwamy FIFO
        }
        free(cashiers_threads);  // Zwolnienie pamieci
        exit(0);  // Zakonczenie programu
    }
    //obsluga sygnalu kierownika signal1 (ewakuacja)
    if(sig == SIGUSR1){
        stop_program = 1;
        printf(RED"\n      EWAKUACJA\n\n"RESET);
        // Zatrzymanie watkow kasjerow
        for (int i = 0; i < CASHIER_AM; i++) {
            pthread_cancel(cashiers_threads[i]);
            pthread_join(cashiers_threads[i], NULL);  // Czekanie na zakonczenie watkow
        }
        // Usuniecie potokow
        for (int i = 0; i < CASHIER_AM; i++) {
            unlink(cashier_stall[i].stall_name);  // Usuwamy FIFO
        }
        free(cashiers_threads);  // Zwolnienie pamieci
        exit(0);  // Zakonczenie programu
    }
    //obsluga sygnalu kierownika signal2
    if(sig == SIGUSR2){
        printf(YELLOW"\n      Otrzymano sygnal o inwenteryzacji\n\n"RESET);
        //wyswietlenie stanu kas
        flg_inwent = 1; //zmiana tej flagi powoduje inne zachowanie w przypadku zakonczenia programu
    }
}

int init_cashier_stalls() {
    for (int i = 0; i < CASHIER_AM; i++) {
        //stworzenie nazwy kasy
        snprintf(cashier_stall[i].stall_name, sizeof(cashier_stall[i].stall_name), "./kasa_%d", i + 1);
        // Usun istniejący plik i utworz nowy FIFO
        unlink(cashier_stall[i].stall_name);  // Usun FIFO, jesli istnieje
        //tworzenie stanowiska fifo na klientow
        if (mkfifo(cashier_stall[i].stall_name, 0600) == -1) {
            perror("mkfifo");
            exit(1);
        }
        //otwarcie fifo dla kas
        int fifo_fd = open(cashier_stall[i].stall_name, O_RDWR);
        if(fifo_fd == -1){
            perror("open FIFO for writing/reading");
            exit(1);
        }
    }
}

// Funkcja kasjera - dodawanie produktow klientow do lokalnej listy i liczenie zyskow
// Ustawia semafory klientow - co umozliwia klientom zwolnienie swoich fifo i swoich semaforow
void *cashier_thread(void *arg) {
    char client_fifo_path[64]; // Sciezka do potoku klienta

    Cashier_stall *cashier_stall = (Cashier_stall *)arg;

    while (!stop_program) {
        ClientData client_data; // Struktura do odczytu danych klienta

        int stall_fifo_fd = open(cashier_stall->stall_name, O_RDONLY | O_NONBLOCK);
        if (stall_fifo_fd == -1) {
            perror("open FIFO for reading");
            exit(1);
        }

        ssize_t cli_fifo_read = read(stall_fifo_fd, &client_fifo_path, sizeof(client_fifo_path));

        if (cli_fifo_read == -1) {
            if (errno == EAGAIN) {
                // FIFO jest puste ponowna proba po pewnym czasie
                close(stall_fifo_fd);
                sleep(rand() % MAX_SLEEP + 1); // Czekanie na dane
                continue;
            } else {
                // Inny błąd podczas odczytu
                perror("read() failed");
                close(stall_fifo_fd);
                exit(1);
            }
        }

        if (cli_fifo_read == 0) {
            // EOF, brak danych w FIFO (powinna nastapic synchronizacja)
            printf("Kasjer %d: FIFO jest puste\n", cashier_stall->stall_id);
            close(stall_fifo_fd);
            sleep(rand() % MAX_SLEEP + 1); // Czekanie na dane
            continue;
        }
        //symulacja opoznienia odczytu danych zakupow klienta
        sleep(rand() % MAX_SLEEP + 1);  // Losowe opoznienie

        // jesli odczytano dane, zakoncz odczyt
        client_fifo_path[cli_fifo_read] = '\0'; // dodanie zakonczenia ciagu
        printf("Kasjer %d: Odczytano dane "YELLOW"%s\n"RESET, cashier_stall->stall_id, client_fifo_path);
        close(stall_fifo_fd);

        // otwarcie FIFO klienta i wczytanie danych zakupow
        int client_fifo_fd = open(client_fifo_path, O_RDONLY | O_NONBLOCK);
        if (client_fifo_fd == -1) {
            perror("open FIFO client");
            exit(EXIT_FAILURE);
        }

        // odczyt danych z FIFO
        ssize_t cli_data_bytes_read = read(client_fifo_fd, &client_data, sizeof(ClientData));
        if (cli_data_bytes_read == -1) {
            //jesli -1
            if (errno == EAGAIN) {
                // FIFO jest puste ponowna proba czytania
                printf("FIFO klienta %s jest puste, ponowna proba odczytu.\n", client_fifo_path);
                close(client_fifo_fd);
                sleep(rand() % MAX_SLEEP + 1);
                continue; // powrot do glownej petli
            } else {
                perror("read from FIFO");
                close(client_fifo_fd);
                exit(EXIT_FAILURE);
            }
        }

        if (cli_data_bytes_read == 0) {
            // read zwrocilo 0, tzn EOF
            printf("Brak danych w FIFO klienta %s\n", client_fifo_path);
            close(client_fifo_fd);
            continue;
        }

        // sprawdzenie liczby zwroconych bajtow
        if (cli_data_bytes_read < sizeof(ClientData)) {
            //w razie odczytu innej wartosci bajtow niz oczekiwano
            printf("Odczytano mniej danych niz spodziewano: %zd bajtow zamiast %zu\n", cli_data_bytes_read, sizeof(ClientData));
        }
        // wypisanie danych odczytanych z FIFO klienta - i zapis do struktury
        printf("Odczytywanie danych od "YELLOW"klienta %d:\n"RESET, client_data.client_id);
        printf("  ID klienta: "YELLOW"%d\n"RESET, client_data.client_id);
        //printf("  ID semafora klienta: %d\n", client_data.client_semid);
        //printf("  Ścieżka FIFO klienta: %s\n", client_data.fifo_path);
        printf("   Zakupione produkty:\n");
	    float cli_cart_cost = 0;
        for (int i = 0; i < NUM_PRODUCTS; i++) {//zapis do struktury kasy
	        if(client_data.products_purchased[i] > 0){
                //wypisanie ile produktow danego typu kupiono
                printf("  Produkt "YELLOW"%d"RESET": "GREEN"%d\n"RESET, i + 1, client_data.products_purchased[i]);

                float product_cost  = local_dispensers[i].price * (float)client_data.products_purchased[i];
                cli_cart_cost += product_cost;
                printf(GREEN"    Koszt produktow: %0.2f\n"RESET,product_cost);
                //zapis kosztu i ilosci do struktury
                cashier_stall->product[i].total_price += product_cost;
                cashier_stall->product[i].sold_amount += client_data.products_purchased[i];
	        }
        }
        printf(GREEN"Calkowity koszt koszyka klienta: %0.2f\n\n"RESET,cli_cart_cost);
        //zamkniecie potoku
        close(client_fifo_fd);
        //dostep do semafora klienta
        int client_sem = client_data.client_semid;
        int semid = semget(client_sem,1,0600);
        if(semid == -1) {
            perror("semget failed");
            exit(1);
        }
        //zmiana wartosci semafora klienta
        //klient moze usunac swoje dane i swoje fifo
        sem_post_op(semid);
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
    sharedPids = (PidsData *)pids_shm_ptr;  // Odczyt danych z pamięci
    //Zapis pidu do pamieci
    sharedPids->kasjer = getpid();
    //Zapisywanie danych do pamieci dzielonej
    
    //jesli w pamieci dzielonej nie ma PID'u piekarza to znaczy ze nie uruchomiono programu w dobrej kolejnosci
    if(sharedPids->piekarz == 0){
        printf(RED"\nUruchomiono program w zlej kolejnosci - kolejnosc: piekarz.c, kasjer.c, klient.c, kierownik.c\n"RESET);
        //Odlaczanie pamieci dzielonej
        if(shmdt(pids_shm_ptr) == -1){
            perror("shmdt");
            return EXIT_FAILURE;
        }
        //Usuwanie pamieci dzielonej
        if(shmctl(pids_shm_id, IPC_RMID,NULL) == -1){
            perror("shmctl");
            return EXIT_FAILURE;
        }
        printf(GREEN"Kasjer: Program zakonczony.\n"RESET);
        return 0;
    }
	//Odlaczanie pamieci dzielonej
	if(shmdt(pids_shm_ptr) == -1){
        perror("shmdt");
        return EXIT_FAILURE;
    }
    //-------------

    // Ustawienie obsługi sygnału SIGINT (Ctrl+C)
    signal(SIGINT, handle_sigint);
    //inicjalizacja kas
    init_cashier_stalls();
    //Tworzenie pamieci dzielonej dla danych produktow i cen (dla kasjera)
    shm_id = shmget(CENNIK_SHM, sizeof(Dispenser), IPC_CREAT | 0600);
    if(shm_id == -1){
	    perror("shmget");
	    return EXIT_FAILURE;
	}
    //wskaznik do pamieci
    shm_ptr = shmat(shm_id, NULL, 0);
    if(shm_ptr == (void *) -1){
	    perror("shmat");
	    return EXIT_FAILURE;
    }
    //Test odczytu danych
    dispensers = (Dispenser *)shm_ptr; //Odczyt danych z pamieci
    for(int i = 0; i<NUM_PRODUCTS;i++){
	    local_dispensers[i] = dispensers[i];//kopiowanie danych z pamieci
    }
    printf("Odczytywanie danych z piekarz, wczytanych przez kasjera \n");
    for (int i = 0; i < NUM_PRODUCTS; i++) {
        printf("Podajnik %d:\n", i + 1);
        printf("  sciezka FIFO: %s\n", local_dispensers[i].fifo_path);
        printf("  Poczatkowy stan: %d\n", local_dispensers[i].initial_stock);
        printf("  Cena: %.2f\n", local_dispensers[i].price);
    }
    // Tworzenie watkow dla kasjerow
    //tworzenie semafora dla usuniecia pamieci po odczycie danych
    int prices_sem = semget(SEM_PRICES,1,0600);
    if(prices_sem == -1){
        perror("semget failed");
        exit(1);
    }
    //odlaczanie kasjer.c od pamieci dzielonej piekarz.c
    if(shmdt(shm_ptr)==-1){
        perror("shmdt");
        exit(1);
    }
    //zmiana wartosci semafora piekarz.c - piekarz moze usunac pamiec, tzn. kasjer odcztal dane i juz nie jest potrzebna
    sem_post_op(prices_sem);

    cashiers_threads = (pthread_t *)malloc(CASHIER_AM * sizeof(pthread_t));
    for (int i = 0; i < CASHIER_AM; i++) {
        cashier_stall[i].stall_id = i+1;
        //zerowanie lacznego zysku z produktow przy inicjalizacji
	    memset(cashier_stall[i].product,0,sizeof(cashier_stall[i].product));
        pthread_create(&cashiers_threads[i], NULL, cashier_thread, &cashier_stall[i]);
    }
    // Program dziala w tle, kasjerzy gotowi do czytania
    printf(BLUE"Kasjer: Program uruchomiony. Naciśnij Ctrl+C, aby zakończyć.\n"RESET);

    // Program bedzie dzialal do momentu, gdy nie otrzyma sygnalu SIGINT - lub sygnalu zamkniecia od kierownik.c
    while (!stop_program) {
        sleep(1);  // Glowna petla czeka na sygnal zakonczenia
    }
    // Sprzatanie przed zakonczeniem
    free(cashiers_threads);  // Zwolnienie pamieci
    // Usuniecie potokow kasjerow (kas)
    for (int i = 0; i < CASHIER_AM; i++) {
        unlink(cashier_stall[i].stall_name);  // Usuwamy FIFO
    }

    printf(GREEN"Kasjer: Program zakonczony.\n"RESET);

    return 0;
}
