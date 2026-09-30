# QNX Automotive Control System

A modular automotive control-system simulation developed using **QNX Neutrino RTOS** and **C**. The project demonstrates inter-process communication (IPC) between independent automotive services using QNX native message-passing mechanisms.

The system currently simulates:

- Wheel Speed Monitoring
- Anti-lock Braking System (ABS)
- Traction Control System (TCS)
- Dashboard Monitoring

The project is designed around independent QNX services that communicate through **name-based IPC**, making the architecture modular and extensible.

---

## 📌 Project Overview

Modern automotive systems consist of multiple independent Electronic Control Units (ECUs) and software services that continuously exchange sensor data and control decisions.

This project demonstrates a simplified version of such an architecture using QNX.

The system follows a service-oriented communication model:

```text
                    ┌─────────────────┐
                    │   Wheel Speed   │
                    │     Service     │
                    └────────┬────────┘
                             │
                    Wheel Speed Data
                             │
               ┌─────────────┴─────────────┐
               │                           │
               ▼                           ▼
       ┌───────────────┐           ┌──────────────────┐
       │      ABS      │           │ Traction Control │
       │    Service    │           │     Service      │
       └───────┬───────┘           └────────┬─────────┘
               │                            │
               │ Braking                    │ Traction
               │ Decision                   │ Decision
               │                            │
               └─────────────┬──────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │    Dashboard    │
                    │     Service     │
                    └─────────────────┘