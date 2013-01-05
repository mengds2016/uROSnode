/*
Copyright (c) 2012-2013, Politecnico di Milano. All rights reserved.

Andrea Zoppi <texzk@email.it>
Martino Migliavacca <martino.migliavacca@gmail.com>

http://airlab.elet.polimi.it/
http://www.openrobots.com/

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * @file    app.c
 * @author  Andrea Zoppi <texzk@email.it>
 *
 * @brief   Application source code.
 */

/*===========================================================================*/
/* HEADER FILES                                                              */
/*===========================================================================*/

#include <urosBase.h>
#include <urosUser.h>
#include <urosNode.h>

#include <math.h>

#include "app.h"
#include "urosTcpRosHandlers.h"

/*===========================================================================*/
/* TYPES & MACROS                                                            */
/*===========================================================================*/

#define min(a,b)    (((a) <= (b)) ? (a) : (b))
#define max(a,b)    (((a) >= (b)) ? (a) : (b))
#define _2PI        ((float)(2.0 * M_PI))

/*===========================================================================*/
/* GLOBAL VARIABLES                                                          */
/*===========================================================================*/

fifo_t rosoutQueue;

turtle_t turtles[MAX_TURTLES];

static UROS_STACKPOOL(turtlesThreadStacks, TURTLE_THREAD_STKSIZE, MAX_TURTLES);
UrosMemPool turtlesMemPool;
UrosThreadPool turtlesThreadPool;

uros_bool_t turtleCanSpawn = UROS_FALSE;
UrosMutex turtleCanSpawnLock;

/*===========================================================================*/
/* GLOBAL FUNCTIONS                                                          */
/*===========================================================================*/

/*~~~ FIFO MESSAGE QUEUE ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void fifo_init(fifo_t *queuep, unsigned length) {

  urosAssert(queuep != NULL);
  urosAssert(length > 0);

  urosSemObjectInit(&queuep->freeSem, length);
  urosSemObjectInit(&queuep->usedSem, 0);
  queuep->length = length;
  queuep->head = 0;
  queuep->tail = 0;
  urosMutexObjectInit(&queuep->slotsMtx);
  queuep->slots = urosArrayNew(length, void *);
  urosAssert(queuep->slots != NULL);
}

void fifo_enqueue(fifo_t *queuep, void *msgp) {

  urosAssert(queuep != NULL);
  urosAssert(msgp != NULL);

  urosSemWait(&queuep->freeSem);
  urosMutexLock(&queuep->slotsMtx);
  queuep->slots[queuep->tail] = msgp;
  if (++queuep->tail >= queuep->length) {
    queuep->tail = 0;
  }
  urosMutexUnlock(&queuep->slotsMtx);
  urosSemSignal(&queuep->usedSem);
}

void *fifo_dequeue(fifo_t *queuep) {

  void *msgp;

  urosAssert(queuep != NULL);

  urosSemWait(&queuep->usedSem);
  urosMutexLock(&queuep->slotsMtx);
  msgp = queuep->slots[queuep->head];
  if (++queuep->head >= queuep->length) {
    queuep->head = 0;
  }
  urosMutexUnlock(&queuep->slotsMtx);
  urosSemSignal(&queuep->freeSem);
  return msgp;
}

/*~~~ ROSOUT ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void rosout_post(UrosString *strp, uros_bool_t costant, uint8_t level,
                 const char *fileszp, int line, const char *funcp) {

  static uint32_t seq = 0;

  struct msg__rosgraph_msgs__Log *msgp;

  urosAssert(urosStringIsValid(strp));

  msgp = urosNew(struct msg__rosgraph_msgs__Log);
  urosAssert(msgp != NULL);
  init_msg__rosgraph_msgs__Log(msgp);

  msgp->header.frame_id = urosStringAssignZ(costant ? "1" : "0");
  msgp->header.seq = seq++;
  msgp->header.stamp.sec = urosGetTimestampMsec();
  msgp->header.stamp.nsec = (msgp->header.stamp.sec % 1000) * 1000000;
  msgp->header.stamp.sec /= 1000;
  msgp->level = level;
  msgp->name = urosNode.config.nodeName;
  msgp->msg = *strp;
  msgp->file = urosStringAssignZ(fileszp);
  msgp->function = urosStringAssignZ(funcp);
  msgp->line = line;

  fifo_enqueue(&rosoutQueue, (void *)msgp);
}

void rosout_fetch(struct msg__rosgraph_msgs__Log **msgpp) {

  urosAssert(msgpp != NULL);

  *msgpp = (struct msg__rosgraph_msgs__Log *)fifo_dequeue(&rosoutQueue);
}

/*~~~ APPLICATION ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void app_initialize(void) {

  static const UrosString turtle1 = { 7, "turtle1" };

  unsigned i;

  /* Initialize the uROS system.*/
  urosInit();

  /* Initialize the /rosout queue.*/
  fifo_init(&rosoutQueue, 8);

  /* Initialize the turtle slots.*/
  urosMutexObjectInit(&turtleCanSpawnLock);
  turtleCanSpawn = UROS_TRUE;
  turtle_init_pools();
  for (i = 0; i < MAX_TURTLES; ++i) {
    turtle_init(&turtles[i], i);
  }

  /* Spawn the first turtle.*/
  turtle_spawn(&turtle1, 0.5f * SANDBOX_WIDTH, 0.5f * SANDBOX_HEIGHT, 0.0f);
}

/*~~~ TURTLE ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

void turtle_init_pools(void) {

  urosMemPoolObjectInit(&turtlesMemPool,
                        sizeof(void*) + TURTLE_THREAD_STKSIZE,
                        NULL);

  urosMemPoolLoadArray(&turtlesMemPool, turtlesThreadStacks, MAX_TURTLES);

  urosThreadPoolObjectInit(&turtlesThreadPool, &turtlesMemPool,
                           (uros_proc_f)turtle_brain_thread, "turtle_brain",
                           TURTLE_THREAD_PRIO);

  urosThreadPoolCreateAll(&turtlesThreadPool);
}

void turtle_init(turtle_t *turtlep, unsigned id) {

  urosAssert(turtlep != NULL);

  urosMutexObjectInit(&turtlep->lock);
  turtlep->id = id;
  urosStringObjectInit(&turtlep->name);
  urosStringObjectInit(&turtlep->poseTopic);
  urosStringObjectInit(&turtlep->velTopic);
  urosStringObjectInit(&turtlep->setpenService);
  urosStringObjectInit(&turtlep->telabsService);
  urosStringObjectInit(&turtlep->telrelService);
  turtlep->pose.x = 0;
  turtlep->pose.y = 0;
  turtlep->pose.theta = 0;
  turtlep->pose.linear_velocity = 0;
  turtlep->pose.angular_velocity = 0;
  turtlep->countdown = 0;
  turtlep->status = TURTLE_EMPTY;
  turtlep->refCnt = 0;
}

uros_err_t turtle_brain_thread(turtle_t *turtlep) {

  struct msg__turtlesim__Pose *posep;

  urosAssert(turtlep != NULL);

  /* Simple integration.*/
  posep = (struct msg__turtlesim__Pose *)&turtlep->pose;
  urosMutexLock(&turtlep->lock);
  while (turtlep->status == TURTLE_ALIVE) {
    /* Execute commands until their deadline.*/
    if (turtlep->countdown > 0) {
      --turtlep->countdown;
      posep->x += (float)cos(posep->theta) * posep->linear_velocity
                  * (0.001f * TURTLE_THREAD_PERIOD_MS);
      posep->y += (float)sin(posep->theta) * posep->linear_velocity
                  * (0.001f * TURTLE_THREAD_PERIOD_MS);
      posep->theta += posep->angular_velocity
                      * (0.001f * TURTLE_THREAD_PERIOD_MS);

      /* Clamp values.*/
      if (posep->x < 0 || posep->x > SANDBOX_WIDTH ||
          posep->y < 0 || posep->y > SANDBOX_WIDTH) {
        static const UrosString msg = { 19, "Turtle hit the wall" };
        rosout_warn((UrosString*)&msg, UROS_TRUE);
      }
      posep->x = min(max(0, posep->x), SANDBOX_WIDTH);
      posep->y = min(max(0, posep->y), SANDBOX_HEIGHT);
      while (posep->theta < 0)     { posep->theta += _2PI; }
      while (posep->theta >= _2PI) { posep->theta -= _2PI; }
    } else {
      posep->linear_velocity = 0;
      posep->angular_velocity = 0;
    }
    urosMutexUnlock(&turtlep->lock);
    urosThreadSleepMsec(TURTLE_THREAD_PERIOD_MS);
    urosMutexLock(&turtlep->lock);
  }
  turtle_unref(turtlep);
  urosMutexUnlock(&turtlep->lock);
  return UROS_OK;
}

turtle_t *turtle_spawn(const UrosString *namep,
                       float x, float y, float theta) {

  static const char *posend = "/pose";
  static const char *velend = "/command_velocity";
  static const char *setpenend = "/set_pen";
  static const char *telabsend = "/teleport_absolute";
  static const char *telrelend = "/teleport_relative";

  turtle_t *turtlep, *curp;
  unsigned i, numAlive;
  uros_err_t err;
  UrosString name, posname, velname, setpenname, telabsname, telrelname;

  urosAssert(urosStringNotEmpty(namep));

  /* Check if the turtle can be spawned.*/
  urosMutexLock(&turtleCanSpawnLock);
  if (!turtleCanSpawn) {
    urosMutexUnlock(&turtleCanSpawnLock);
    return NULL;
  }
  urosMutexUnlock(&turtleCanSpawnLock);

  /* Fill an empty slot.*/
  for (turtlep = NULL, numAlive = 0; turtlep == NULL;) {
    for (i = 0, curp = turtles; i < MAX_TURTLES; ++curp, ++i) {
      urosMutexLock(&curp->lock);
      if (curp->status == TURTLE_ALIVE) {
        urosError(0 == urosStringCmp(&curp->name, namep),
                  { urosMutexUnlock(&curp->lock); return NULL; },
                  ("A turtle named [%.*s] is alive\n", UROS_STRARG(namep)));
        ++numAlive;
      }
      if (curp->status == TURTLE_EMPTY) {
        turtlep = curp;
        break;
      }
      urosMutexUnlock(&curp->lock);
    }
    if (numAlive == MAX_TURTLES) {
      /* All the turtles are alive, sorry.*/
      return NULL;
    }
    if (turtlep == NULL) {
      /* Wait for 10ms to let referencing threads release a slot.*/
      urosThreadSleepMsec(10);
    }
  }

  /* Build the topic names.*/
  name = urosStringClone(namep);
  if (name.datap == NULL) { return NULL; }
  urosStringObjectInit(&posname);
  urosStringObjectInit(&velname);
  urosStringObjectInit(&setpenname);
  urosStringObjectInit(&telabsname);
  urosStringObjectInit(&telrelname);

  posname.length = 1 + namep->length + strlen(posend);
  posname.datap = (char*)urosAlloc(posname.length + 1);
  velname.length = 1 + namep->length + strlen(velend);
  velname.datap = (char*)urosAlloc(velname.length + 1);
  setpenname.length = 1 + namep->length + strlen(setpenend);
  setpenname.datap = (char*)urosAlloc(setpenname.length + 1);
  telabsname.length = 1 + namep->length + strlen(telabsend);
  telabsname.datap = (char*)urosAlloc(telabsname.length + 1);
  telrelname.length = 1 + namep->length + strlen(telrelend);
  telrelname.datap = (char*)urosAlloc(telrelname.length + 1);
  if (posname.datap == NULL || velname.datap == NULL ||
      setpenname.datap == NULL || telabsname.datap == NULL ||
      telrelname.datap == NULL) {
    urosStringClean(&posname);
    urosStringClean(&velname);
    urosStringClean(&setpenname);
    urosStringClean(&telabsname);
    urosStringClean(&telrelname);
    return NULL;
  }

  posname.datap[0] = '/';
  memcpy(1 + posname.datap, namep->datap, namep->length);
  memcpy(1 + posname.datap + namep->length, posend, strlen(posend) + 1);
  velname.datap[0] = '/';
  memcpy(1 + velname.datap, namep->datap, namep->length);
  memcpy(1 + velname.datap + namep->length, velend, strlen(velend) + 1);
  setpenname.datap[0] = '/';
  memcpy(1 + setpenname.datap, namep->datap, namep->length);
  memcpy(1 + setpenname.datap + namep->length, setpenend, strlen(setpenend) + 1);
  telabsname.datap[0] = '/';
  memcpy(1 + telabsname.datap, namep->datap, namep->length);
  memcpy(1 + telabsname.datap + namep->length, telabsend, strlen(telabsend) + 1);
  telrelname.datap[0] = '/';
  memcpy(1 + telrelname.datap, namep->datap, namep->length);
  memcpy(1 + telrelname.datap + namep->length, telrelend, strlen(telrelend) + 1);

  /* Assign the new attributes.*/
  turtlep->name = urosStringClone(namep);
  turtlep->poseTopic = posname;
  turtlep->velTopic = velname;
  turtlep->setpenService = setpenname;
  turtlep->telabsService = telabsname;
  turtlep->telrelService = telrelname;
  turtlep->pose.x = min(max(0, x), SANDBOX_WIDTH);
  turtlep->pose.y = min(max(0, y), SANDBOX_HEIGHT);
  turtlep->pose.theta = theta;
  while (turtlep->pose.theta < 0)     { turtlep->pose.theta += _2PI; }
  while (turtlep->pose.theta >= _2PI) { turtlep->pose.theta -= _2PI; }
  turtlep->pose.linear_velocity = 0;
  turtlep->pose.angular_velocity = 0;
  turtlep->countdown = 0;
  turtlep->status = TURTLE_ALIVE;
  turtlep->refCnt = 1; /* For the brain thread only.*/

  /* Publish "<turtle>/pose" .*/
  err = urosNodePublishTopicSZ(posname.datap,
                               "turtlesim/Pose",
                               (uros_proc_f)pub_tpc__turtleX__pose);
  urosError(err != UROS_OK,
            goto _error,
            ("Error %s while publishing topic [%s]\n",
             urosErrorText(err), posname.datap));

  /* Subscribe to "<turtle>/command_velocity".*/
  err = urosNodeSubscribeTopicSZ(velname.datap,
                                 "turtlesim/Velocity",
                                 (uros_proc_f)sub_tpc__turtleX__command_velocity);
  urosError(err != UROS_OK,
            { urosNodeUnpublishTopic(&posname);
              goto _error; },
            ("Error %s while subscribing to topic [%s]\n",
             urosErrorText(err), velname.datap));

  /* Publish "<turtle>/set_pen".*/
  err = urosNodePublishServiceSZ(setpenname.datap,
                                 "turtlesim/SetPen",
                                 (uros_proc_f)pub_srv__turtleX__set_pen);
  urosError(err != UROS_OK,
            { urosNodeUnpublishTopic(&posname);
              urosNodeUnpublishTopic(&velname);
              goto _error; },
            ("Error %s while publishing service [%s]\n",
             urosErrorText(err), setpenname.datap));

  /* Publish "<turtle>/teleport_absolute".*/
  err = urosNodePublishServiceSZ(telabsname.datap,
                                 "turtlesim/TeleportAbsolute",
                                 (uros_proc_f)pub_srv__turtleX__teleport_absolute);
  urosError(err != UROS_OK,
            { urosNodeUnpublishTopic(&posname);
              urosNodeUnpublishTopic(&velname);
              urosNodeUnpublishService(&setpenname);
              goto _error; },
            ("Error %s while publishing service [%s]\n",
             urosErrorText(err), telabsname.datap));

  /* Publish "<turtle>/teleport_relative".*/
  err = urosNodePublishServiceSZ(telrelname.datap,
                                 "turtlesim/TeleportRelative",
                                 (uros_proc_f)pub_srv__turtleX__teleport_relative);
  urosError(err != UROS_OK,
            { urosNodeUnpublishTopic(&posname);
              urosNodeUnpublishTopic(&velname);
              urosNodeUnpublishService(&setpenname);
              urosNodeUnpublishService(&telabsname);
              goto _error; },
            ("Error %s while publishing service [%s]\n",
             urosErrorText(err), telrelname.datap));

  /* Start its new brain.*/
  err = urosThreadPoolStartWorker(&turtlesThreadPool, (void*)turtlep);
  urosAssert(err == UROS_OK);
  urosMutexUnlock(&turtlep->lock);
  return turtlep;

_error:
  turtlep->status = TURTLE_EMPTY;
  turtlep->refCnt = 0;
  urosStringClean(&posname);
  urosStringClean(&velname);
  urosStringClean(&setpenname);
  urosStringClean(&telabsname);
  urosStringClean(&telrelname);
  urosMutexUnlock(&turtlep->lock);
  return NULL;
}

void turtle_kill(turtle_t *turtlep) {

  uros_err_t err;

  urosAssert(turtlep != NULL);
  urosAssert(turtlep->status == TURTLE_ALIVE);

  /* Unpublish its topics.*/
  err = urosNodeUnpublishTopic(&turtlep->poseTopic);
  urosError(err != UROS_OK, UROS_NOP,
            ("Error %s while unpublishing topic [%.*s]\n",
             urosErrorText(err), UROS_STRARG(&turtlep->poseTopic)));

  err = urosNodeUnsubscribeTopic(&turtlep->velTopic);
  urosError(err != UROS_OK, UROS_NOP,
            ("Error %s while unsubscribing topic [%.*s]\n",
             urosErrorText(err), UROS_STRARG(&turtlep->velTopic)));

  /* Unpublish its services.*/
  err = urosNodeUnpublishService(&turtlep->setpenService);
  urosError(err != UROS_OK, UROS_NOP,
            ("Error %s while unpublishing service [%.*s]\n",
             urosErrorText(err), UROS_STRARG(&turtlep->setpenService)));

  err = urosNodeUnpublishService(&turtlep->telabsService);
  urosError(err != UROS_OK, UROS_NOP,
            ("Error %s while unpublishing service [%.*s]\n",
             urosErrorText(err), UROS_STRARG(&turtlep->telabsService)));

  err = urosNodeUnpublishService(&turtlep->telrelService);
  urosError(err != UROS_OK, UROS_NOP,
            ("Error %s while unpublishing service [%.*s]\n",
             urosErrorText(err), UROS_STRARG(&turtlep->telrelService)));

  /* Cleanup fields.*/
  urosMutexLock(&turtlep->lock);
  urosStringClean(&turtlep->name);
  urosStringClean(&turtlep->poseTopic);
  urosStringClean(&turtlep->velTopic);
  urosStringClean(&turtlep->setpenService);
  urosStringClean(&turtlep->telabsService);
  urosStringClean(&turtlep->telrelService);
  turtlep->status = TURTLE_DEAD;
  urosMutexUnlock(&turtlep->lock);
}

turtle_t *turtle_refbyname(const UrosString *name) {

  turtle_t *turtlep;
  unsigned i;

  urosAssert(urosStringNotEmpty(name));

  /* Find the turtle by its name.*/
  for (turtlep = turtles, i = 0; i < MAX_TURTLES; ++turtlep, ++i) {
    urosMutexLock(&turtlep->lock);
    if (0 == urosStringCmp(name, &turtlep->name)) {
      ++turtlep->refCnt;
      urosMutexUnlock(&turtlep->lock);
      return turtlep;
    }
    urosMutexUnlock(&turtlep->lock);
  }
  return NULL;
}

turtle_t *turtle_refbypath(const UrosString *topicName) {

  turtle_t *turtlep;
  unsigned i;

  urosAssert(urosStringNotEmpty(topicName));
  urosAssert(topicName->datap[0] == '/');

  /* Find the turtle by its topic/service path.*/
  for (turtlep = turtles, i = 0; i < MAX_TURTLES; ++turtlep, ++i) {
    urosMutexLock(&turtlep->lock);
    if (topicName->length >= 1 + turtlep->name.length + 1 &&
        topicName->datap[1 + turtlep->name.length] == '/' &&
        0 == memcmp(topicName->datap + 1, turtlep->name.datap,
                    turtlep->name.length)) {
      ++turtlep->refCnt;
      urosMutexUnlock(&turtlep->lock);
      return turtlep;
    }
    urosMutexUnlock(&turtlep->lock);
  }
  return NULL;
}

void turtle_unref(turtle_t *turtlep) {

  urosAssert(turtlep != NULL);
  urosAssert(turtlep->refCnt > 0);

  /* Note: Must be locked! */
  if (--turtlep->refCnt == 0) {
    urosAssert(turtlep->status == TURTLE_DEAD);
    turtlep->status = TURTLE_EMPTY;
  }
}
