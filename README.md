# chat
Простой сервер-демон.
## Installation
    make server
## Run
### Интерактивный режим (вывод в консоль)
    ./run_i.sh
или 

    ./server -i -pid <Путь к pid файлу>
Pid файл сервер создаст сам.
### Режим демона (Требуется указать, куда писать лог)
    ./run_d.sh
лог будет в той же папке, что и сервер, или

    ./server -pid <Путь к pid файлу> -l <путь к лог файлу>
pid файл сервер создаст сам. Лог файл будет дозаписываться, если его не существует, он будет создан.


Для закрытия сервера в режиме демона следет использовать скрипт `kill.sh`
