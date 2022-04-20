# 2020Fall_CN_NetworkStorageSystem

A simple implementation of network storage system.
- client uploads/downloads files to/from server
- video streaming
- multiple connections

## Usage
### Build
```
make server
make client
```
### Launch
```
./server [port]
./client [ip:port]
```
### Commands
- print out files in the server's folder
  ```
  ls
  ```
- upload file to the server's folder
  ```
  put [filename]
  ```
- download file to the client's folder
  ```
  get [filename]
  ```
- play video (must be a .mpg file)
  ```
  play [videofile]
  ```
