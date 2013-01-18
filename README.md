# ngx_http_self_defence_module

## Introduction

A Nginx module make nginx in self-defence according to the 
values in shared memory. A external script can periodically
change the values in shared memory according to the gathered
information of the server machine, such as free memory, 
load average, swap ratio, bandwidth and so on. 
`ngx_http_self_defence` will make internal redirect according
to the value and matched action in config.

## Synopsis

    http {
        defence_shm      12345 10;
    
        server {
            listen       80  default deferred;
            server_name  localhost;
    
            defence_at       1;
            defence_action   1   @internal  100;
            defence_action   2   /defence   20;
    
            location @internal {
                return 500;
            }
    
            location /defence {
                return 503;
            }
    
            location / {
                root   html;
                index  index.html index.htm;
            }
        }
    }
        

## Diretives

* **syntax** : ***defence_shm*** shm_key [shm_length]
* **default**: --
* **context**: http

Set SYSV shared memory key and length used in defence module.
The shm_length must between 0(excluding) and 255(including).
If no 'shm_length' supplied, use 8 as default;


* **syntax** : ***defence_at*** pos
* **default**: defence_at 0
* **context**: http, server, location

Specify the offset in shared memory of the byte whose value the module will
detect to trigger the action handler specified in ***defence_action***.


* **syntax** : ***defence_action*** value [action] [ratio]
* **default**: --
* **context**: http, server, location

Specify the value of shared memory. When matched with 'value' parameter,
the request will be redirected to the URL specified by the 'action' parameter.
`Nginx` will return 503 if no 'action' URL provided.

If no 'ratio' specified, all request will trigger the action. Otherwise, only
ratio percentage will trigger the action. The 'ratio' must be between 0 and 
100. 

A external script should periodically update the value in shared memory, then
the `Nginx` can make it in self-defence according the values.


## Limits

This module is only tested on Linux machine.

## Author

FengGu <flygoast@126.com>
