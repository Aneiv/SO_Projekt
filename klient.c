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

#define NUM_PRODUCTS 11   // Liczba podajnikow
#define CASHIER_AM 3 //max liczba kasjerow - synchronizacja dostepu do kas miedzy klientami
#define MAX_CLIENTS 1000 //max liczba klientow, gdy osiagnie wartosc program sie konczy
//pamiec dzielona na pidy dla kierownik.c
#define PID_SHM 555
//pamiec dzielona na l. akt. kli. dla kierownik.c
#define CLIAMO_SHM 444
//pamiec dzielona dla zarzadzania kasami 2-3
#define SHM_STALLS 777

// Funkcja do operacji sem_wait (czekanie na semafor)
void sem_wait_op(int semid) {
    struct sembuf sops;
    sops.sem_num = 0;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = -1;    // Operacja dekrementacji (czekanie na semafor)
    sops.sem_flg = 0;    // Flagi (brak)

    // Czekamy na semafor (jeśli jego wartość to 0)
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

    // Zwalniamy semafor (zwiększamy jego wartosc)
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

    // Czekamy na semafor (jeśli jego wartość to 0)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_wait_grp)");
        exit(1);
    }
}
// Funkcja do operacji sem_post (zwalnianie semafora) DLA GRUPY SEMAFOROW
void grp_sem_post_op(int semid, int sem_num) {
    struct sembuf sops;
    sops.sem_num = sem_num;    // Numer semafora w zestawie (w tym przypadku semafor 0)
    sops.sem_op = 1;           // Operacja inkrementacji (zwalnianie semafora)
    sops.sem_flg = 0;          // Flagi (brak)

    // Zwalniamy semafor (zwiększamy jego wartosc)
    if (semop(semid, &sops, 1) == -1) {
        perror("semop failed (sem_post_grp)");
        exit(1);
    }
}

// Struktura danych klienta
typedef struct {
    int client_id;                 // ID klienta
    int products_purchased[NUM_PRODUCTS]; // Liczba produktow pobranych z kazdego podajnika
    char fifo_path[64];                   //sciezka do potoku
    int client_semid;                     //id semafora dla klienta
    int semid;                            //id semafora klienta (uzyskane)
} ClientData;

int IDclient = 0; //licznik id_klientow - do tworzenia klientow o unikalnym ID
int stop_program = 0;  // Flaga zakonczenia programu

int* active_clients= NULL;//Liczba aktywnych klientow
pthread_mutex_t clients_mutex; //Mutex do synchronizacji dostepu do zmiennej wyzej (active_clients)

pthread_t *clients_threads; //tablica watkow klientow
ClientData client_data[MAX_CLIENTS];
//semafor dostepu do zapisu do kas
int stall_sem = 88;
int stall_sem_id;
//flaga zamkniecia tworzenia klientow
int crt_client = 0;
//Tworzenie grupy semaforow dla podajnikow (dispensers)
int dispensers_sem = 100;
int dispensers_sem_id;
int flg_ewak = 0;
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
//id do pamieci dzielonej z l. akt. klientow
int cliamo_shm_id;
//--------------------
// Struktura przechowująca dwie flagi do kontroli dostepu do kas 2-3
typedef struct {
    int m_kasa2;
    int m_kasa3;
    int tmp_stop; //czasowe zatrzymanie tworzenia klientow(przekroczono dop. l. kli.)
    int kli_ewak; //wiadomosc dla kierownik.c ze moze zwolnic pozostale programy bo juz nie ma klientow
} KasyMutex;
KasyMutex *ctl_m_stalls; //pamiec dzielona na dostep do kas
//ctl_m_stalls->m_kasa2 = 1;  // Przykładowa operacja przypisania
//ctl_m_stalls->m_kasa3 = 1;  // Przykładowa operacja przypisania
//-----------------------

// Funkcja obsługi sygnału SIGINT (Ctrl+C)
void handle_sigint(int sig) {
    if(sig == SIGINT){
        stop_program = 1;
        printf("\nKlient: Zatrzymywanie programu...\n");

        for(int i = 0; i < IDclient;i++){
            pthread_cancel(clients_threads[i]);
            pthread_join(clients_threads[i],NULL);
        }

        //zwalnianie grupy semaforow synchronizacji kas
        if(semctl(stall_sem_id,0,IPC_RMID)==-1){
            perror("semctl IPC_RMID failed");
            exit(1);
        }
        // Zwolnienie mutexu po zakończeniu użycia
        if (pthread_mutex_destroy(&clients_mutex) != 0) {
            perror("Failed to destroy mutex");
        }
        //Odlaczenie sie od pamieci z kontrola kas 2-3
        if (shmdt(ctl_m_stalls) == -1) {
            perror("shmdt");
        }
        free(clients_threads); //zwalnianie pamieci dla tablicy watkow
        exit(0);
    }
    //obsluga sygnalu kierownika signal1 - ewakuacja
    if(sig == SIGUSR1){
        stop_program = 1;
        crt_client = 1;
        flg_ewak = 1;
        printf(RED"\n      EWAKUACJA\n\n"RESET);
        printf(GREEN"Klienci odkladaja towary i opuszczaja sklep"RESET"\n");
    }
    //signal2 dla klienta dziala inaczej
    //obsluga sygnalu kierownika signal2 (zakonczenie produkcji klientow)
    if(sig == SIGUSR2){
        crt_client = 1;
        printf("\n      Zamkniecie piekarni - wstrzymanie wpuszczania klientow\n\n");
    }
}
//funkcja klienta do wybierania kas z dostepnych (kontrola dostepu - kierownik.c)
int choose_stall(int k2, int k3) {
    int available[3];  // Tablica do przechowywania dostępnych kas
    int count = 0;     // Licznik dostępnych kas

    available[count++] = 1;  // Kasa 1 jest zawsze dostępna

    if (k2 == 1) {
        available[count++] = 2;  // Dodajemy kase 2, jesli jest wlaczona
    }
    if (k3 == 1) {
        available[count++] = 3;  // Dodajemy kase 3, jesli jest wlaczona
    }
    int choice = available[rand() % count];  // Losowy wybor sposrod dostepnych
    return choice;
}

// Funkcja watku klienta
// - tworzenie potoku klienta ("klient_id")
// - pobranie w petli z podajnikow kilku roznych produktow jesli sa dostepne we wlasciwej ilosci
// - zapisanie do swojego fifo danych pobranych towarow
// - po otrzymaniu zmiany semafora od kasjer.c usuwa swoje fifo i swoje dane
void *client_thread(void *arg) {
    //mechanizm blokady mutexem zmiennej z liczba klientow w sklepie
    pthread_mutex_lock(&clients_mutex);
    (*active_clients)++;
    pthread_mutex_unlock(&clients_mutex);

    //tworzenie nazw semaforow
    char sem_name[20];
    //tworzenie semafora
    ClientData *client_data = (ClientData *)arg;

    snprintf(client_data->fifo_path, sizeof(client_data->fifo_path), "./klient_%d",client_data->client_id);

    //zapisanie nazwy semafora danego watku
    snprintf(sem_name,sizeof(sem_name),"500%d",client_data->client_id);

    //zapisanie id_semafora klienta do jego struktury
    client_data->client_semid = atoi(sem_name);
    int semid;
    //tworzenie semafora watku wartosc p. 0
    if(stop_program == 0){
        semid = semget(atoi(sem_name),1,IPC_CREAT | 0600);
        if(semid == -1){
            perror("semget failed");
            exit(1);
        }
        //inicjalizacja semafora na 0
        if(semctl(semid,0,SETVAL,0)==-1){
            perror("semctl SETVAL failed");
            exit(1);
        }
        //zapis do struktury semid (aktywne)
        client_data->semid = semid;
    }
    int num_of_var = rand() % 3 + 2;//liczba roznych produktow zakres 2-4

    while (!stop_program) {
        for(int h = 0; h<num_of_var;h++){
            if(stop_program == 1){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
                break;
            }
            int product_index = rand() % NUM_PRODUCTS;  // Wybor losowego produktu (index)
            int quantity_to_take = rand() % 3 + 1;     // Losowa liczba produktow do pobrania
            // Otworz potok FIFO dla wybranego produktu
            char fifo_path[64];
            snprintf(fifo_path, sizeof(fifo_path), "./podajnik_%d", product_index + 1);
            if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
                //blokowanie semaforem dostepu do podajnika            
                grp_sem_wait_op(dispensers_sem_id,product_index);//indexy semaforow kas od 0
                int fifo_fd_read = open(fifo_path, O_RDONLY);		
                if (fifo_fd_read == -1) {
                    perror("Klient: Błąd otwarcia FIFO");
                    grp_sem_post_op(dispensers_sem_id,product_index);
                    sleep(1);
                    continue;
                }

                // Pobierz aktualną liczbę produktów
                int available_stock;
                //jak odczyta i zamknie to zostanie puste
                if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
                    ssize_t bytes_read = read(fifo_fd_read, &available_stock, sizeof(available_stock));
                    if (bytes_read <= 0 || available_stock < quantity_to_take) {
                        printf(BLUE"Klient %d: Brak wystarczającej liczby produktow w podajniku %d.\n"RESET,
                            client_data->client_id, product_index + 1);
                            // Otwarcie potoku podajnika do zapisu
                            int fifo_fd_write = open(fifo_path, O_WRONLY);          
                            if(fifo_fd_write == -1){
                                    perror("open FIFO for writing");
                                    grp_sem_post_op(dispensers_sem_id,product_index);
                                    continue;
                            }
                            ssize_t bytes_written = write(fifo_fd_write, &available_stock, sizeof(available_stock));
                            if (bytes_written == -1) {
                                perror("Klient: Błąd zapisu do FIFO");
                                grp_sem_post_op(dispensers_sem_id,product_index);
                            }
                        close(fifo_fd_write);
                        close(fifo_fd_read);
                        //odblokowanie semaforem dostepu do podajnika
                        grp_sem_post_op(dispensers_sem_id,product_index);
                        sleep(rand() % 2 + 1);  // Losowe opoznienie
                        continue;
                    }//if available_stock < quantity_to_take
                }
                // Zaktualizuj liczbe produktow
                available_stock -= quantity_to_take;

                // Otwarcie potoku podajnika do zapisu
                int fifo_fd_write = open(fifo_path, O_WRONLY);		
                if(fifo_fd_write == -1){
                        perror("open FIFO for writing");
                        grp_sem_post_op(dispensers_sem_id,product_index);
                        continue;
                }
                ssize_t bytes_written = write(fifo_fd_write, &available_stock, sizeof(available_stock));
                if (bytes_written == -1) {
                    perror("Klient: Błąd zapisu do FIFO");
                    grp_sem_post_op(dispensers_sem_id,product_index);
                } else {
                    if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
                        // Aktualizuj dane klienta
                        client_data->products_purchased[product_index] += quantity_to_take;
                        printf("Klient %d: Pobrano %d produktów z podajnika %d. Pozostało: %d.\n",
                        client_data->client_id, quantity_to_take, product_index + 1, available_stock);
                    }
                }
                close(fifo_fd_write);
                close(fifo_fd_read);
                //odblokowanie semaforem dostepu do podajnika      
                grp_sem_post_op(dispensers_sem_id,product_index);
                //symulacja czasu zakupow klienta
                sleep(rand() % 4 + 1);  // Losowe opóźnienie
            }
        }//for
        break;
    }//main while
    int buy = 0;
    if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
        // Wyswietl podsumowanie zakupow po zakonczeniu
        printf(GREEN"\nKlient %d: Podsumowanie zakupow:\n"RESET, client_data->client_id);
        //sprawdzanie czy klient kupil cokolwiek
        
        for (int i = 0; i < NUM_PRODUCTS; i++) {
            if (client_data->products_purchased[i] > 0 && stop_program == 0) {
                buy++; //zmienna - sprawdzenie czy klient cos kupil
                printf("  Produkty z Podajnik %d: %d produktow\n", i + 1, client_data->products_purchased[i]);
            }
        }
    }
    printf("\n");
    //zapisanie danych klienta do potoku nazwanego klienta ('klient_id')
    //id klienta bierzemy z client_data->client_id
    //usuwamy fifo jesli juz takie istnieje
    unlink(client_data->fifo_path);
    //tworzymy fifo
    if(mkfifo(client_data->fifo_path, 0600) == -1){
        perror("mkfifo");
        exit(1);
    }
    if(buy == 0){//jesli nic nie kupil
        //zamkniecie fifo klienta
        //zwalnianie semafora wybranego klienta
        if(semctl(semid,0,IPC_RMID)==-1){
            perror("semctl IPC_RMID failed");
            exit(1);
        }
        //usuniecie potoku klienta
        unlink(client_data->fifo_path);
        if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
            printf(YELLOW"Klient %d nie znalazl potrzebnych towarow, opuszcza piekarnie\n"RESET,client_data->client_id);
            sleep(rand() % 4 + 1);  // Losowe opoznienie
        }
        else{
            printf(YELLOW"Klient %d opuszcza piekarnie\n"RESET,client_data->client_id);
        }
        //zwalnianie klienta ze sklepu
        //mechanizm blokady mutexem zmiennej z liczba klientow w sklepie
        pthread_mutex_lock(&clients_mutex);
        (*active_clients)--;
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    //zapisywanie do potoku klienta informacji o zakupach klienta (./klient_x)
    int cli_fifo_fd = open(client_data->fifo_path, O_RDWR);
    if(cli_fifo_fd == -1){
        perror("open FIFO for writing/reading");
        exit(1);
    }
    // Zapisz dane klienta do FIFO
    ssize_t bytes_written = write(cli_fifo_fd, client_data, sizeof(ClientData));
    if (bytes_written == -1) {
        perror("write to FIFO");
        close(cli_fifo_fd);
        exit(1);
    } else if (bytes_written != sizeof(ClientData)) {
        fprintf(stderr, "Zapisano niepelne dane do FIFO: oczekiwano %zu bajtow, zapisano %zd\n", sizeof(ClientData), bytes_written);
    }
   	//klient tworzy semafor i czeka az zwiekszy sie jego wartosc
	//wartosc zwieksza kasjer.c po tym jak skasuje produkty klienta
	//sprawi to ze klient bedzie mogl zwolnic swoje dane, semafor i fifo klient_x
    //komunikacja z kasjer.c - zapisanie klienta na liste oczekujacych

    char kasa[20];//jak 0 w m_kasa to nie mozna jej wybrac
    //kasa wybierana z dostepnych
    int nr_kasy = choose_stall(ctl_m_stalls->m_kasa2,ctl_m_stalls->m_kasa3);
    //wybranie kasy
    snprintf(kasa, sizeof(kasa), "./kasa_%d",nr_kasy);

    if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
    //synchronizacja semaforem dostepu do potoku kasy
    //blokowanie semafora przed dostepem do potoku kasy
        grp_sem_wait_op(stall_sem_id,nr_kasy-1);//indexy semaforow kas od 0

        int fifo_stall = open(kasa, O_WRONLY | O_NONBLOCK);
        if (fifo_stall == -1) {
            perror("Klient: Blad otwarcia FIFO");
            grp_sem_post_op(stall_sem_id,nr_kasy-1); //zwolnienie semafora
            sleep(1);
            exit(1);
        }
        //Zapisanie nazwy potoku klienta do kasy
        if(write(fifo_stall,&client_data->fifo_path, sizeof(client_data->fifo_path)) == -1){
            perror("write to FIFO");
                grp_sem_post_op(stall_sem_id,nr_kasy-1); //zwolnienie semafora
            close(fifo_stall);
            exit(1);
        }

        close(fifo_stall);
        grp_sem_post_op(stall_sem_id, nr_kasy-1); //zwolnienie semafora po zapisie do kasy
    }
    if(stop_program == 0){//sprawdzenie wystapienia zatrzymania - nie wiemy w ktorym miejscu jest dany watek klienta
    //czekamy na sem_post() od kasjer.c (informacja ze klient zostal obsluzony przez kase)
        sem_wait_op(semid);

        //zwalnianie semafora po obsluzeniu(usuwanie semafora dla wybranego klienta)
        if(semctl(semid,0,IPC_RMID)==-1){
            perror("semctl IPC_RMID failed");
            exit(1);
        }
    }
    //Mutex sync, zwolnienie klienta po obsluzeniu przez kasjer.c
    pthread_mutex_lock(&clients_mutex);
    (*active_clients)--;
    pthread_mutex_unlock(&clients_mutex);

    //zamykamy po operacjach './klient_x'
    close(cli_fifo_fd);

    //usuniecie pliku potoku
    unlink(client_data->fifo_path);
    return NULL;
}//client_thread end

int client_chance(){//losowanie szansy na pojawienie sie klienta
    int rand_num = rand()%100;
    if(rand_num < 60){return 1;}
    else {return 0;}
}

int main() {
    srand(time(NULL));// inicjalizacja generatora liczb losowych

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
    sharedPids->klient = getpid();
    //Zapisywanie danych do pamieci dzielonej

    //jesli w pamieci dzielonej nie ma PID'u piekarza lub kasjera to znaczy ze nie uruchomiono programu w dobrej kolejnosci
    if(sharedPids->piekarz == 0 || sharedPids->kasjer == 0){
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
        printf(GREEN"Klient: Program zakonczony.\n"RESET);
        return 0;
    }

    //--------------------
    //Dostep do pamieci zarzadzania dostepem do kas 2-3
    int stalls_id = shmget(SHM_STALLS, sizeof(KasyMutex), IPC_CREAT | 0600);
    if (stalls_id == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    ctl_m_stalls = (KasyMutex *)shmat(stalls_id, NULL, 0);
    if (ctl_m_stalls == (void *)-1) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }
    //poczatkowe wartosci zmiennych 0
    ctl_m_stalls->m_kasa2 = 0;
    ctl_m_stalls->m_kasa3 = 0;
    ctl_m_stalls->tmp_stop = 0;
    ctl_m_stalls->kli_ewak = 0;
    //--------------------


    //--------------------
    // Rejestracja obsługi sygnalow
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

    //---------------------------
    //Tworzenie pamieci dzielonej dla l. aktywnych klientow dla kierownika
    cliamo_shm_id = shmget(CLIAMO_SHM, sizeof(int), IPC_CREAT | 0600);
    if(cliamo_shm_id == -1){
	    perror("shmget");
	    return EXIT_FAILURE;
	}
    //wskaznik do pamieci
    active_clients = (int*)shmat(cliamo_shm_id, NULL, 0);
    if(active_clients == (int *) -1){
	    perror("shmat");
	    return EXIT_FAILURE;
    }
    //-------------------------------


	//Odlaczanie pamieci dzielonej z PID'ami procesow po zapisie
	if(shmdt(pids_shm_ptr) == -1){
        perror("shmdt");
        return EXIT_FAILURE;
    }
    //-------------
    // Rejestracja obsługi SIGINT
    signal(SIGINT, handle_sigint);
    //tworzenie mutexu
    pthread_mutex_init(&clients_mutex, NULL);
    clients_threads = (pthread_t *)malloc(MAX_CLIENTS * sizeof(pthread_t));
    
    //tworzenie 3 semaforow dla synch. zapisu do 3 kas
    stall_sem_id = semget(stall_sem,CASHIER_AM,IPC_CREAT | 0600);
    if(stall_sem_id == -1){
        perror("semget failed");
        exit(1);
    }
    //domyslna wartosc semaforow 1
    for(int i = 0; i < CASHIER_AM; i++){
	    if(semctl(stall_sem_id,i,SETVAL,1)==-1){
            perror("semctl SETVAL failed");
            exit(1);
    	}
    }
    //Uzyskujemy dostep do grupy semaforow do synchronizacji podajnikow
	dispensers_sem_id = semget(dispensers_sem,NUM_PRODUCTS,0600);
	if(dispensers_sem_id == -1) {
		perror("semget failed");
		exit(1);
	}

    while (!stop_program && (IDclient < MAX_CLIENTS) && crt_client == 0) {
        //czasowe zatrzymanie prod. klientow (N >= max)
        while(ctl_m_stalls->tmp_stop == 1 && stop_program == 0){
            sleep(1);
            printf("\n\nPrzekroczono max. l. klientow w piekarni\n");
            printf("Oczekiwanie na zwolnienie miejsca\n\n");
            continue;
        }

        //klient pojawia sie z okreslona szansa
        int chance = client_chance();
        if (chance){
            // Tworzenie watkow klientow i ich danych
            client_data[IDclient].client_id = IDclient+1;
            memset(client_data[IDclient].products_purchased, 0, sizeof(client_data[IDclient].products_purchased));
            pthread_create(&clients_threads[IDclient], NULL, client_thread, &client_data[IDclient]);
            IDclient++;
            //brak sleep() - co jakis czas fale klientow
            //sleep(rand() % 2 + 1);  // Losowe opoznienie
	    }
        else{
            sleep(rand() % 2 + 1);  // Losowe opoznienie
        }
    }//while stop

    // Oczekiwanie na zakonczenie watkow
    for (int i = 0; i < IDclient; i++) {
        pthread_join(clients_threads[i], NULL);
    }

    //zwalnianie grupy semaforow synchronizacji kas
    if(semctl(stall_sem_id,0,IPC_RMID)==-1){
        perror("semctl IPC_RMID failed");
        exit(1);
    }

    //odlaczanie pamieci dzielonej l. akt. kli. dla kierownik.c
    // Odlaczenie pamieci dzielonej
    if (shmdt(active_clients) == -1) {
        perror("shmdt");
        exit(EXIT_FAILURE);
    }
    ctl_m_stalls->kli_ewak = 1; //wiadomosc dla kierownik.c ze moze zwolnic pozostale programy bo juz nie ma klientow
    //Odlaczenie sie od pamieci z kontrola kas 2-3
    if (shmdt(ctl_m_stalls) == -1) {
        perror("shmdt");
    }
    //Zwolnienie mutexu po zakonczeniu uzycia
    if (pthread_mutex_destroy(&clients_mutex) != 0) {
        perror("Failed to destroy mutex");
    }
    free(clients_threads); //zwalnianie pamieci dla tablicy watkow
    printf(GREEN"Klient: Program zakończony.\n"RESET);
    return 0;
}
