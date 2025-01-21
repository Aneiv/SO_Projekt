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


//pamiec dzielona na pidy dla kierownik.c
#define PID_SHM 555
//sygnaly kierownika
#define SG1 SIGUSR1 //signal1 - EWAKUACJA zakonczenie pracy klientow i pozostalych procesow
#define SG2 SIGUSR2 //signal2 - podsumowanie (kasy, piekarze + produkty niesprzedane(ktore zostaly na podajnikach))
//#define SG2 SIGUSR2 //signal2 - w przypadku klienta  - dla zakonczenia tworzenia klientow 
//max czas dzialania programu - potem zwalnia pliki
#define MAX_RUNTIME 100
//pamiec dzielona na l. akt. kli. dla kierownik.c
#define CLIAMO_SHM 444
//pamiec dzielona na dla zarzadzania dostepem do kas 2-3
#define SHM_STALLS 777
//maksymalna l. klientow w piekarni na raz
#define MAX_CRT_CLI 60
//--------------------
// Struktura przechowująca dwie flagi do kontroli dostepu do kas 2-3
typedef struct {
    int m_kasa2;//0 - zamknieta / 1 - wlacz kase
    int m_kasa3;//0 - zamknieta / 1 - wlacz kase
    //czasowe zatrzymanie tworzenia klientow(przekroczono dop. l. kli.)
    int tmp_stop; //0 - tworzenie kli. / 1 - zatrzymanie prod
    int kli_ewak; //wartosc zwraca klient.c gdy wszyscy klienci oposcili piekarnie
    // informacja dla kierownik.c ze mozna zwolnic pozostale programy
} KasyMutex;
//-----------------------

// Struktura pamieci z pidami procesow
typedef struct {
    pid_t piekarz;
    pid_t kasjer;
    pid_t klient;
} PidsData;


//zmienna kontroli ctrl+z
int sig_stp = 0;
//PAMIEC DLA PIDOW
//wskaznik do pamieci dzielonej dla PID'ow
void *pids_shm_ptr;
//id do pamieci dzielonej dla PID'ow
int pids_shm_id;
//Struktura do odczytu z pamieci PIDow
PidsData *sharedPids;
PidsData localPids;
//------
//id do pamieci dzielonej z l. akt. klientow
int cliamo_shm_id;
//wskaznik do pamieci z l. akt. klientow
int *active_clients = NULL;
//przestrzen na pamiec do kontroli otwarcia kas (m_kasa) i zablokowania klientow przed wchodzeniem do piekarni (tmp_stop)
KasyMutex *ctl_m_stalls;
//identyfikator pamieci dzielonej do kontroli kas 2-3
int stalls_id; 

// Funkcja obslugi sygnalu SIGINT (Ctrl+C) (ewakuacja - wysyla sygnal ewakuacji do reszty procesow)
void handle_sigint(int sig) {
    if(sig == SIGINT){
        //w razie zatrzymania w trakcie przekroczenia max kli. w piekarni
        ctl_m_stalls->tmp_stop = 0;
        printf(RED"\n\nEWAKUACJA - awaryjne zamkniecie piekarni\n\n"RESET);
        if (kill(localPids.klient, SG1) == -1) {
            perror("Nie udalo się wyslac sygnalu");
        } else {
            printf("Wyslano sygnal zakonczenia do procesu klient\n");
        }
        //oczekiwanie na zwolnienie klientow
        while(ctl_m_stalls->kli_ewak == 0){
            sleep(1);
        }
        if (kill(localPids.kasjer, SG1) == -1) {
            perror("Nie udalo się wyslac sygnalu");
        } else {
            printf("Wyslano sygnal zakonczenia do procesu kasjer\n");
        }
        if (kill(localPids.piekarz, SG1) == -1) {
            perror("Nie udalo się wyslac sygnalu");
        } else {
            printf("Wyslano sygnal zakonczenia do procesu piekarz\n");
        }
        //zwalnianie struktur
        //Odlaczanie pamieci dzielonej l. akt. klientow
        if(shmdt(active_clients) == -1){
            perror("shmdt");
            exit(1);
        }
        //Usuwanie pamieci dzielonej l. akt. klientow
        if(shmctl(cliamo_shm_id, IPC_RMID,NULL) == -1){
            perror("shmctl");
            exit(1);
        }

        //Zwalnianie pamieci do kontroli kas 2-3
        // Odlaczenie pamieci dzielonej
        if (shmdt(ctl_m_stalls) == -1) {
            perror("shmdt");
        }

        // Usuniecie segmentu pamięci dzielonej do kontroli kas 2-3
        if (shmctl(stalls_id, IPC_RMID, NULL) == -1) {
            perror("shmctl");
        }
        //------------
        printf("Zwolniono pamiec\n");
        exit(0);
    }
    //inwenteryzacja reczna (Ctrl + Z), drugi raz to zamykanie reczne sklepu przed czasem konca (nie ewakuacja)
    if(sig == SIGTSTP){
        if(sig_stp == 0){
            //wyslanie sygnalow o inwenteryzacji dla kasjer i piekarz
            if (kill(localPids.kasjer, SG2) == -1) {
                perror("Nie udalo się wyslac sygnalu");
            } else {
                printf("Wyslano sygnal 2 do procesu kasjer\n");
            }
            if (kill(localPids.piekarz, SG2) == -1) {
                perror("Nie udalo się wyslac sygnalu");
            } else {
                printf("Wyslano sygnal 2 do procesu piekarz\n");
            }
            sig_stp++;//zwiekszenie zmiennej kontroli
        }
        //zatrzymanie piekarni reczne przed czasem
        else if(sig_stp == 1){
            //w razie zatrzymania w trakcie przekroczenia max kli. w piekarni
            ctl_m_stalls->tmp_stop = 0;
            printf(YELLOW"\nZamykanie piekarni przed czasem\n"RESET);
            printf("Odsylanie pracownikow i zamykanie sklepu...\n");
            //wysylanie sygnalow zamkniecia do pracownikow i klientow
            if (kill(localPids.klient, SG2) == -1) {
                perror("Nie udalo się wyslac sygnalu");
            } else {
                printf("Wyslano sygnal zakonczenia do procesu klient - zamkniecie wpuszczania nowych klientow\n");
            }
            while(1){//while do sprawdzania ile klientow zostalo w piekarni
            
                if(*active_clients == 0){
                    if (kill(localPids.kasjer, SIGINT) == -1) {
                        perror("Nie udalo się wyslac sygnalu");
                    } else {
                        printf("Wyslano sygnal zakonczenia do procesu kasjer\n");
                    }
                    if (kill(localPids.piekarz, SIGINT) == -1) {
                        perror("Nie udalo się wyslac sygnalu");
                    } else {
                        printf("Wyslano sygnal zakonczenia do procesu piekarz\n");
                    }
                    break;
                }
                else{
                    printf("liczba akt. klientow "YELLOW"%d"RESET"\n", *active_clients);
                    sleep(1);
                    continue;
                }
            }
            sleep(1);
            //po zakonczeniu pracy kierownik usuwa pamiec dzielona 
            //Odlaczanie pamieci dzielonej - zapis liczby akt. klientow
            if(shmdt(active_clients) == -1){
                perror("shmdt");
                exit(1);
            }
            //Usuwanie pamieci dzielonej - zapis liczby akt. klientow
            if(shmctl(cliamo_shm_id, IPC_RMID,NULL) == -1){
                perror("shmctl");
                exit(1);
            }
            printf("Zwolniono pamiec l. klientow\n");

            //------------
            //Zwalnianie pamieci do kontroli kas 2-3
            // Odlaczenie pamieci dzielonej
            if (shmdt(ctl_m_stalls) == -1) {
                perror("shmdt");
            }

            // Usuniecie segmentu pamieci dzielonej
            if (shmctl(stalls_id, IPC_RMID, NULL) == -1) {
                perror("shmctl");
            }
            //------------
            exit(0);
        }
    }
}

int main() {
    // Rejestracja obsługi SIGINT
    signal(SIGINT, handle_sigint);
    //Obsluga SIGTSTP (Ctrl + Z) do sygnału inwenteryzacji 
    signal(SIGTSTP, handle_sigint);

    //tworzenie pamieci dzielonej i usuwanie jej po odczytaniu i zapisaniu danych
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
    //Zapisywanie danych do pamieci dzielonej
    memcpy(&localPids,sharedPids,sizeof(PidsData));
    //jesli w pamieci dzielonej nie ma wszystkich PID'u to znaczy ze nie uruchomiono programu w dobrej kolejnosci
    if(sharedPids->piekarz == 0 || sharedPids->kasjer == 0 || sharedPids->klient == 0){
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
        printf(GREEN"Kierownik: Program zakończony.\n"RESET);
        return 0;
    }
    else{
        printf("Odczytano dane\nZamykanie pamieci dzielonej\n");
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
        //----------------

        printf("Odczytane dane z pamieci:\n");
        printf("Pid piekarz.c: %d\n",localPids.piekarz);
        printf("Pid kasjer.c: %d\n",localPids.kasjer);
        printf("Pid klient.c: %d\n",localPids.klient);
    }

    //inicjalizacja i dostep do zmiennych dla kas 2-3
    // Pobranie identyfikatora pamięci dzielonej
    stalls_id = shmget(SHM_STALLS, sizeof(KasyMutex), 0600);
    if (stalls_id == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }
    // Przypisanie pamieci dzielonej do przestrzeni adresowej
    ctl_m_stalls = (KasyMutex *)shmat(stalls_id, NULL, 0);
    if (ctl_m_stalls == (void *)-1) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }

    //zapisanie czasu
    time_t start_time, current_time;
    time(&start_time);
    //Dostep do pamieci z l. akt. kli. 
    // Pobranie identyfikatora pamieci dzielonej
    cliamo_shm_id = shmget(CLIAMO_SHM, sizeof(int), 0600);
    if (cliamo_shm_id == -1) {
        perror("shmget");
        exit(1);
    }

    // Przypisanie pamieci dzielonej do przestrzeni adresowej
    active_clients = (int*)shmat(cliamo_shm_id, NULL, 0);
    if (active_clients == (int *)-1) {
        perror("shmat");
        exit(1);
    }
    //-------------------

    //petla programu kierownik.c
    while(1){
        //sprawdzenie czasu
        time(&current_time);
        printf("\n\nliczba akt. klientow "YELLOW"%d\n"RESET, *active_clients);
        //obliczanie roznicy czasu
        double elapsed = difftime(current_time, start_time);
        double remaining = MAX_RUNTIME - elapsed;
        if(*active_clients >= MAX_CRT_CLI){
            //blokujemy mozliwosc tworzenia nowych klientow a potem odblokowujemy
            ctl_m_stalls->tmp_stop = 1;
            printf(RED"\n\nPrzekroczono dop. liczbe klientow (%d), klienci czekaja na zewnatrz\n\n"RESET,MAX_CRT_CLI);
        }
        else{
            ctl_m_stalls->tmp_stop = 0;
        }
        //warunki otwarcia kas: K < N/3 - otwarta tylko kasa 1
        if(*active_clients < MAX_CRT_CLI/3){
            ctl_m_stalls->m_kasa2 = 0;
            ctl_m_stalls->m_kasa3 = 0;
        }
        //warunki otwarcia kas: K >= N/3 && K < 2N/3 otwarta kasa 1 i 2
        else if(*active_clients >= MAX_CRT_CLI/3 && *active_clients < 2 * MAX_CRT_CLI / 3){
            ctl_m_stalls->m_kasa2 = 1;
            ctl_m_stalls->m_kasa3 = 0;
        }
        //warunki otwarcia kas: K >= N/3 && K < 2N/3 otwarta kasa 1 i 2 i 3
        else if(*active_clients >= 2 * MAX_CRT_CLI / 3){
            ctl_m_stalls->m_kasa2 = 1;
            ctl_m_stalls->m_kasa3 = 1;
        }
        printf("\nStan kas (1 - otwarta): "BLUE"kasa2: %d\tkasa3: %d\n\n"RESET,ctl_m_stalls->m_kasa2,ctl_m_stalls->m_kasa3);
        printf("Czas dzialania piekarni: "BLUE"%.2lf\n"RESET,elapsed);
        printf("Czas pozostaly do zamkniecia piekarni: "BLUE "%.2lf\n"RESET,remaining);
        if(elapsed >= MAX_RUNTIME){
            printf("\nMinal czas pracy piekarni\n");
            printf("Odsylanie pracownikow i zamykanie sklepu...\n");
            //w razie zatrzymania w trakcie przekroczenia max kli. w piekarni
            ctl_m_stalls->tmp_stop = 0;
            //wysylanie sygnalow zamkniecia do pracownikow i klientow
            if (kill(localPids.klient, SG2) == -1) {
                perror("Nie udalo sie wyslac sygnalu");
            } else {
                printf("Wyslano sygnal zakonczenia do procesu klient - zamkniecie wpuszczania nowych klientow\n");
            }
            while(1)//while do sprawdzania ile klientow zostalo w piekarni
            {
                if(*active_clients == 0){
                    if (kill(localPids.kasjer, SIGINT) == -1) {
                        perror("Nie udalo sie wyslac sygnalu");
                    } else {
                        printf("Wyslano sygnal zakonczenia do procesu kasjer\n");
                    }
                    if (kill(localPids.piekarz, SIGINT) == -1) {
                        perror("Nie udalo sie wyslac sygnalu");
                    } else {
                        printf("Wyslano sygnal zakonczenia do procesu piekarz\n");
                    }
                    break;
                }
                else{
                    printf("liczba akt. klientow "YELLOW"%d\n"RESET, *active_clients);
                    sleep(1);
                    continue;
                }
            }
            if(*active_clients == 0){
                break;
            }
        }
        sleep(1);//opoznienie wiadomosci kierownika
    }
    //po zakonczeniu pracy kierownik usuwa pamiec dzielona - zapis liczby akt. klientow
    //Odlaczanie pamieci dzielonej
	if(shmdt(active_clients) == -1){
        perror("shmdt");
        return EXIT_FAILURE;
    }
	//Usuwanie pamieci dzielonej
	if(shmctl(cliamo_shm_id, IPC_RMID,NULL) == -1){
        perror("shmctl");
        return EXIT_FAILURE;
    }
    printf("Zwolniono pamiec l. klientow\n");

    //------------
    //Zwalnianie pamieci do kontroli kas 2-3
    // Odlaczenie pamieci dzielonej
    if (shmdt(ctl_m_stalls) == -1) {
        perror("shmdt");
    }

    // Usuniecie segmentu pamieci dzielonej
    if (shmctl(stalls_id, IPC_RMID, NULL) == -1) {
        perror("shmctl");
    }
    //------------
    return 0;
}
