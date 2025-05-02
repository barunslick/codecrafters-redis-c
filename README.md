# 🚀 Building Redis in C (CodeCrafters Challenge)

[![progress-banner](https://backend.codecrafters.io/progress/redis/03c77575-b022-4aec-ac25-ec062b9efff2)](https://app.codecrafters.io/users/codecrafters-bot?r=2qF)

The challenge is structured in stages — from handling basic commands to implementing persistence, replication, and streams. Below is a checklist to track my progress through the stages.

---

#### ✅ Base Stages

- [x] Introduction  
- [x] Repository Setup  
- [x] Bind to a port  
- [x] Respond to PING  
- [x] Respond to multiple PINGs  
- [x] Handle concurrent clients  
- [x] Implement the ECHO command  
- [x] Implement the SET & GET commands  
- [x] Expiry  
- [x] Base stages complete!  

---

#### 💾 RDB Persistence

- [x] RDB file config  
- [x] Read a key  
- [x] Read a string value  
- [x] Read multiple keys  
- [x] Read multiple string values  
- [x] Read value with expiry  
- [x] Extension complete!  

---

#### 🔁 Replication

- [x] Configure listening port  
- [x] The INFO command  
- [x] The INFO command on a replica  
- [x] Initial Replication ID and offset  
- [x] Send handshake (1/3)  
- [x] Send handshake (2/3)  
- [x] Send handshake (3/3)  
- [x] Receive handshake (1/2)  
- [x] Receive handshake (2/2)  
- [x] Empty RDB Transfer  
- [ ] Single-replica propagation  
- [ ] Multi Replica Command Propagation  
- [ ] Command Processing  
- [ ] ACKs with no commands  
- [ ] ACKs with commands  
- [ ] WAIT with no replicas  
- [ ] WAIT with no commands  
- [ ] WAIT with multiple commands  

---

#### 📚 Streams

- [ ] The TYPE command  
- [ ] Create a stream  
- [ ] Validating entry IDs  
- [ ] Partially auto-generated IDs  
- [ ] Fully auto-generated IDs  
- [ ] Query entries from stream  
- [ ] Query with `-`  
- [ ] Query with `+`  
- [ ] Query single stream using ID range  
- [ ] Query multiple streams using ID range  
- [ ] Blocking reads  
- [ ] Blocking reads without timeout  
- [ ] Blocking reads using `$`  

---

#### 🔐 Transactions
TODO: Add sections

---


#### References

- https://beej.us/guide/bgnet/html/
- https://redis.io/docs/latest/commands/


> 🗓️ Last updated: April 30, 2025  
> 💡 [Challenge link](https://codecrafters.io/challenges/redis)
