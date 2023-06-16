/*
 * Copyright (c) 2015-2023 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution and use  in source  and binary  forms,  with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 *					Anthony Mallet on Fri Feb 13 2015
 */
#include "acrotorcraft.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <float.h>
#include <fnmatch.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "rotorcraft_c_types.h"
#include "codels.h"

/* supported devices */
static const struct {
  const char *match;		/* match string */
  double rev;			/* minimum revision */
  double gres, ares, mres;	/* imu resolutions */
  double tres, toff;		/* temperature resolution */
} rc_devices[] = {
  [RC_MKBL] = {
    .match = "%*cmkbl%lf", .rev = 1.8,
    .gres = 1/1000., .ares = 1/1000., .mres = 1e-8
  },

  [RC_MKFL] = {
    .match = "mkfl%lf", .rev = 1.8,
    .gres = 1/1000., .ares = 1/1000., .mres = 1e-8
  },

  [RC_FLYMU] = {
    .match = "flymu%lf", .rev = 1.8,
    .gres = 1/1000., .ares = 1/1000., .mres = 1e-8
  },

  [RC_CHIMERA] = {
    .match = "chimera%lf", .rev = 1.1,
    .gres = 1000. * M_PI/180 / 32768,
    .ares = 8 * 9.81 / 32768,
    .mres = 1e-8,
    .tres = 1/333.87, .toff = 21.
  },

  [RC_TEENSY] = {
    .match = "teensy%lf", .rev = 1.1
  },
};


static void	mk_comm_recv_msg(struct mk_channel_s *chan,
                        const rotorcraft_ids_imu_calibration_s *imu_calibration,
                        rotorcraft_ids_imu_filter_s *imu_filter,
                        rotorcraft_ids_sensor_time_s *sensor_time,
                        const rotorcraft_imu *imu, const rotorcraft_mag *mag,
                        rotorcraft_ids_rotor_data_s *rotor_data,
                        rotorcraft_ids_battery_s *battery, double *imu_temp,
                        const genom_context self);
genom_event	mk_connect_chan(const char serial[64], uint32_t baud,
                        struct mk_channel_s *chan, const genom_context self);

static void	rc_filter_imu_data(const double raw[3],
                        const double scale[3*3], const double bias[3],
                        double in[3], const double alpha[3], double out[3]);
static void	mk_get_ts(uint8_t seq, struct timeval atv, double rate,
                        rotorcraft_ids_sensor_time_s_ts_s *timings,
                        or_time_ts *ts, double *lprate);


/* --- Task comm -------------------------------------------------------- */

/** Codel mk_comm_start of task comm.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_poll.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_comm_start(const genom_context self)
{
  (void)self;

  return rotorcraft_poll;
}


/** Codel mk_comm_poll of task comm.
 *
 * Triggered by rotorcraft_poll.
 * Yields to rotorcraft_nodata, rotorcraft_recv.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_comm_poll(const rotorcraft_conn_s *conn, const genom_context self)
{
  struct timeval deadline;
  int s;

  /* 500ms timeout */
  gettimeofday(&deadline, NULL);
  deadline.tv_usec += 500000; /* exceeding 1e6 is not an issue */

  do
    s = mk_wait_msg(conn, &deadline);
  while (s < 0 && errno == EINTR);

  if (s < 0) return mk_e_sys_error(NULL, self);
  if (s == 0) return rotorcraft_nodata;
  return rotorcraft_recv;
}


/** Codel mk_comm_nodata of task comm.
 *
 * Triggered by rotorcraft_nodata.
 * Yields to rotorcraft_poll.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_comm_nodata(rotorcraft_conn_s **conn,
               rotorcraft_ids_imu_filter_s *imu_filter,
               rotorcraft_ids_sensor_time_s *sensor_time,
               const rotorcraft_imu *imu, const rotorcraft_mag *mag,
               rotorcraft_ids_rotor_data_s rotor_data[8],
               rotorcraft_ids_battery_s *battery, double *imu_temp,
               const genom_context self)
{
  or_pose_estimator_state *idata = imu->data(self);
  or_pose_estimator_state *mdata = mag->data(self);
  struct timeval tv;
  int i;

  gettimeofday(&tv, NULL);

  /* reset exported data in case of timeout */
  imu_filter->g[0] = imu_filter->g[1] = imu_filter->g[2] =
  imu_filter->a[0] = imu_filter->a[1] = imu_filter->a[2] =
  imu_filter->m[0] = imu_filter->m[1] = imu_filter->m[2] =
  imu_filter->gf[0] = imu_filter->gf[1] = imu_filter->gf[2] =
  imu_filter->af[0] = imu_filter->af[1] = imu_filter->af[2] =
  imu_filter->mf[0] = imu_filter->mf[1] = imu_filter->mf[2] = nan("");

  idata->ts.sec = tv.tv_sec;
  idata->ts.nsec = tv.tv_usec * 1000;
  idata->avel._present = false;
  idata->avel._value.wx = idata->avel._value.wy = idata->avel._value.wz =
    nan("");
  idata->acc._present = false;
  idata->acc._value.ax = idata->acc._value.ay = idata->acc._value.az =
    nan("");

  mdata->ts.sec = tv.tv_sec;
  mdata->ts.nsec = tv.tv_usec * 1000;
  mdata->att._present = false;
  mdata->att._value.qw =
    mdata->att._value.qy =
    mdata->att._value.qz =
    mdata->att._value.qx = nan("");

  battery->ts.sec = tv.tv_sec;
  battery->ts.nsec = tv.tv_usec * 1000;
  battery->level = nan("");

  *imu_temp = nan("");

  for(i = 0; i < or_rotorcraft_max_rotors; i++)
    rotor_data[i].state = (or_rotorcraft_rotor_state){
      .ts.sec = tv.tv_sec, .ts.nsec = tv.tv_usec * 1000,
      .emerg = 0, .spinning = 0, .starting = 0,
      .disabled = rotor_data[i].state.disabled,
      .velocity = nan(""),
      .throttle = nan(""),
      .consumption = nan(""),
      .energy_level = nan("")
    };

  if (mk_set_sensor_rate(&sensor_time->rate, *conn, NULL, sensor_time, self))
    mk_disconnect_start(conn, self);

  return rotorcraft_poll;
}


/** Codel mk_comm_recv of task comm.
 *
 * Triggered by rotorcraft_recv.
 * Yields to rotorcraft_poll, rotorcraft_recv.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_comm_recv(rotorcraft_conn_s **conn,
             const rotorcraft_ids_imu_calibration_s *imu_calibration,
             rotorcraft_ids_imu_filter_s *imu_filter,
             rotorcraft_ids_sensor_time_s *sensor_time,
             const rotorcraft_imu *imu, const rotorcraft_mag *mag,
             rotorcraft_ids_rotor_data_s rotor_data[8],
             rotorcraft_ids_battery_s *battery, double *imu_temp,
             const genom_context self)
{
  int more;
  uint32_t i;

  for(i = more = 0; i < (*conn)->n; i++)
    if (mk_recv_msg(&(*conn)->chan[i], false) == 1) {
      more = 1;
      mk_comm_recv_msg(&(*conn)->chan[i],
                       imu_calibration, imu_filter, sensor_time,
                       imu, mag, rotor_data, battery, imu_temp,
                       self);
    }

  return more ? rotorcraft_recv : rotorcraft_poll;
}

static void
mk_comm_recv_msg(struct mk_channel_s *chan,
                 const rotorcraft_ids_imu_calibration_s *imu_calibration,
                 rotorcraft_ids_imu_filter_s *imu_filter,
                 rotorcraft_ids_sensor_time_s *sensor_time,
                 const rotorcraft_imu *imu, const rotorcraft_mag *mag,
                 rotorcraft_ids_rotor_data_s *rotor_data,
                 rotorcraft_ids_battery_s *battery, double *imu_temp,
                 const genom_context self)
{
  struct timeval tv;
  size_t i;
  uint8_t *msg, len;
  int16_t v16;
  uint16_t u16;

  gettimeofday(&tv, NULL);

  msg = chan->msg;
  len = chan->len;
  switch(*msg++) {
    case 'I': /* IMU data */
      if (!chan->imu) break;
      if (len == 14 || len == 16) {
        or_pose_estimator_state *idata = imu->data(self);
        double v[3];
        uint8_t seq = *msg++;

        if (seq == sensor_time->imu.seq) break;

        /* accelerometer */
        mk_get_ts(
          seq, tv, sensor_time->rate.imu, &sensor_time->imu,
          &idata->ts, &sensor_time->measured_rate.imu);

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[0] = v16 * rc_devices[chan->device].ares;

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[1] = v16 * rc_devices[chan->device].ares;

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[2] = v16 * rc_devices[chan->device].ares;

        rc_filter_imu_data(
          v, imu_calibration->ascale, imu_calibration->abias,
          imu_filter->a, imu_filter->aalpha, imu_filter->af);

        idata->acc._value.ax = imu_filter->af[0];
        idata->acc._value.ay = imu_filter->af[1];
        idata->acc._value.az = imu_filter->af[2];
        idata->acc._present = true;

        /* gyroscope */
        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[0] = v16 * rc_devices[chan->device].gres;

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[1] = v16 * rc_devices[chan->device].gres;

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[2] = v16 * rc_devices[chan->device].gres;

        rc_filter_imu_data(
          v, imu_calibration->gscale, imu_calibration->gbias,
          imu_filter->g, imu_filter->galpha, imu_filter->gf);

        idata->avel._value.wx = imu_filter->gf[0];
        idata->avel._value.wy = imu_filter->gf[1];
        idata->avel._value.wz = imu_filter->gf[2];
        idata->avel._present = true;

        /* update temperature if present */
        if (len == 16) {
          v16 = ((int16_t)(*msg++) << 8);
          v16 |= ((uint16_t)(*msg++) << 0);

          *imu_temp = v16 * rc_devices[chan->device].tres +
                      rc_devices[chan->device].toff;
        }
      } else
        warnx("bad IMU message");
      break;

    case 'C': /* magnetometer data */
      if (!chan->mag) break;
      if (len == 8) {
        or_pose_estimator_state *mdata = mag->data(self);
        double v[3];
        uint8_t seq = *msg++;

        if (seq == sensor_time->mag.seq) break;

        mk_get_ts(
          seq, tv, sensor_time->rate.mag, &sensor_time->mag,
          &mdata->ts, &sensor_time->measured_rate.mag);

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[0] = v16 * rc_devices[chan->device].mres
               + imu_calibration->mbias[0];

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[1] = v16 * rc_devices[chan->device].mres
               + imu_calibration->mbias[1];

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        v[2] = v16 * rc_devices[chan->device].mres
               + imu_calibration->mbias[2];

        rc_filter_imu_data(
          v, imu_calibration->mscale, imu_calibration->mbias,
          imu_filter->m, imu_filter->malpha, imu_filter->mf);

        mdata->att._value.qw = nan("");
        mdata->att._value.qx = imu_filter->mf[0];
        mdata->att._value.qy = imu_filter->mf[1];
        mdata->att._value.qz = imu_filter->mf[2];
        mdata->att._present = true;
      } else
        warnx("bad magnetometer message");
      break;

    case 'M': /* motor data */
      if (!chan->motor) break;
      if (len == 9) {
        uint8_t seq = *msg++;
        uint8_t state = *msg++;
        uint8_t id = state & 0xf;

        id += chan->minid - 1; /* apply hw offset */
        if (id < chan->minid || id > chan->maxid) break;
        id--;
        if (seq == sensor_time->motor[id].seq) break;

        if (rotor_data[id].autoconf && rotor_data[id].state.disabled)
          rotor_data[id].state.disabled = 0;

        mk_get_ts(
          seq, tv, sensor_time->rate.motor, &sensor_time->motor[id],
          &rotor_data[id].state.ts, &sensor_time->measured_rate.motor);

        rotor_data[id].state.emerg = !!(state & 0x80);
        rotor_data[id].state.spinning = !!(state & 0x20);
        rotor_data[id].state.starting = !!(state & 0x10);

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        if (rotor_data[id].state.spinning)
          rotor_data[id].state.velocity = v16 ? 1e6/2/v16 : 0.;
        else
          rotor_data[id].state.velocity = 0.;

        v16 = ((int16_t)(*msg++) << 8);
        v16 |= ((uint16_t)(*msg++) << 0);
        rotor_data[id].state.throttle = v16 * 100./1023.;

        u16 = ((uint16_t)(*msg++) << 8);
        u16 |= ((uint16_t)(*msg++) << 0);
        rotor_data[id].state.consumption = u16 / 1e3;
      } else
        warnx("bad motor data message");
      break;

    case 'B': /* battery data */
      if (len == 4) {
        uint8_t seq  __attribute__((unused)) = *msg++;
        double p;

        u16 = ((uint16_t)(*msg++) << 8);
        u16 |= ((uint16_t)(*msg++) << 0);
        battery->level = u16/1000.;

        battery->ts.sec = tv.tv_sec;
        battery->ts.nsec = tv.tv_usec * 1000;

        p = 100. *
            (battery->level - battery->min)/(battery->max - battery->min);
        for(i = 0; i < or_rotorcraft_max_rotors; i++)
          rotor_data[i].state.energy_level = p;
      } else
        warnx("bad battery message");
      break;

    case 'T': /* clock rate */
      if (!chan->motor) break;
      if (len == 3) {
        uint8_t id = *msg++;

        id += chan->minid - 1; /* apply hw offset */
        if (id < chan->minid || id > chan->maxid) break;
        id--;
        rotor_data[id].clkrate = *msg;
      } else
        warnx("bad clock rate message");
      break;

    case '?': /* ignored messages */
      break;

    default:
      warnx("received unknown message");
  }
}


/** Codel mk_comm_stop of task comm.
 *
 * Triggered by rotorcraft_stop.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_comm_stop(rotorcraft_conn_s **conn, const genom_context self)
{
  uint32_t i;

  /* stop all streaming */
  mk_set_sensor_rate(
    &(struct rotorcraft_ids_sensor_time_s_rate_s){ 0 }, *conn,
    NULL, NULL, self);

  /* stop motors and close */
  for(i = 0; i < (*conn)->n; i++) {
    if ((*conn)->chan[i].fd < 0) continue;

    mk_send_msg(&(*conn)->chan[i], "x");
    close((*conn)->chan[i].fd);
  }

  if ((*conn)->chan) free((*conn)->chan);
  (*conn)->chan = NULL;
  (*conn)->n = 0;

  return rotorcraft_ether;
}


/* --- Activity connect ------------------------------------------------- */

/** Codel mk_connect_start of activity connect.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys, rotorcraft_e_baddev.
 */
genom_event
mk_connect_start(const char serial[64], uint32_t baud,
                 rotorcraft_conn_s **conn,
                 rotorcraft_ids_sensor_time_s *sensor_time,
                 const genom_context self)
{
  struct mk_channel_s *chan;
  genom_event e;
  uint32_t i;

  chan = malloc(sizeof(*chan));
  if (!chan) return mk_e_sys_error("malloc", self);

  /* disconnect all */
  for(i = 0; i < (*conn)->n; i++)
    if ((*conn)->chan[i].fd >= 0) {
      close((*conn)->chan[i].fd);
      warnx("disconnected from %s", (*conn)->chan[i].path);
    }
  if ((*conn)->chan) free((*conn)->chan);
  (*conn)->chan = NULL;
  (*conn)->n = 0;

  /* open */
  e = mk_connect_chan(serial, baud, chan, self);
  if (e) { free(chan); return e; }

  chan->imu = chan->mag = chan->motor = true;
  chan->minid = 1; chan->maxid = or_rotorcraft_max_rotors;
  (*conn)->chan = chan;
  (*conn)->n = 1;

  /* configure data streaming */
  mk_set_sensor_rate(&sensor_time->rate, *conn, NULL, sensor_time, self);

  return rotorcraft_ether;
}


/* --- Activity pconnect ------------------------------------------------ */

/** Codel mk_pconnect_start of activity pconnect.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys, rotorcraft_e_baddev.
 */
genom_event
mk_pconnect_start(const char serial[64], uint32_t baud, bool imu,
                  bool mag, bool motor, uint16_t offset,
                  rotorcraft_conn_s **conn,
                  rotorcraft_ids_sensor_time_s *sensor_time,
                  const genom_context self)
{
  (void)serial, (void)baud, (void)imu, (void)mag, (void)motor, (void)conn,
    (void)sensor_time, (void)self;

  rotorcraft_e_baddev_detail d;
  struct mk_channel_s *chan;
  uint16_t minid, maxid;
  genom_event e;
  uint32_t i;

  chan = malloc(sizeof(*chan));
  if (!chan) return mk_e_sys_error("malloc", self);

  /* open */
  e = mk_connect_chan(serial, baud, chan, self);
  if (e) { free(chan); return e; }

  /* check already open device */
  for(i = 0; i < (*conn)->n; i++) {
    if ((*conn)->chan[i].fd < 0) continue;
    if ((*conn)->chan[i].st_dev != chan->st_dev) continue;
    if ((*conn)->chan[i].st_ino != chan->st_ino) continue;

    /* disconnect */
    close((*conn)->chan[i].fd);
    (*conn)->chan[i].fd = -1;
  }

  /* check conflicting flags */
  for(i = 0; i < (*conn)->n; i++) {
    if ((*conn)->chan[i].fd < 0) continue;

    if ((imu && (*conn)->chan[i].imu) ||
        (mag && (*conn)->chan[i].mag)) {
      snprintf(d.dev, sizeof(d.dev),
               "conflicting device with `%.128s'", (*conn)->chan[i].path);
      close(chan->fd);
      free(chan);
      return rotorcraft_e_baddev(&d, self);
    }
  }

  /* allocate motor id range */
  minid = maxid = 0;
  if (motor) {
    minid = offset + 1;
    maxid = or_rotorcraft_max_rotors;
    for(i = 0; i < (*conn)->n; i++) {
      if ((*conn)->chan[i].fd < 0) continue;
      if (!(*conn)->chan[i].motor) continue;
      if ((*conn)->chan[i].maxid < minid) continue;

      if ((*conn)->chan[i].minid >= minid) {
        if ((*conn)->chan[i].minid <= maxid)
          maxid = (*conn)->chan[i].minid - 1;
        continue;
      }

      maxid = (*conn)->chan[i].maxid;
      (*conn)->chan[i].maxid = minid - 1;
    }

    if (maxid < minid || minid < 1 || maxid > or_rotorcraft_max_rotors) {
      snprintf(d.dev, sizeof(d.dev),
               "invalid motor range %d-%d", minid, maxid);
      close(chan->fd);
      free(chan);
      return rotorcraft_e_baddev(&d, self);
    }
  }

  /* record */
  chan->imu = imu;
  chan->mag = mag;
  chan->motor = motor;
  chan->minid = minid; chan->maxid = maxid;

  for(i = 0; i < (*conn)->n; i++) {
    if ((*conn)->chan[i].fd >= 0) continue;

    (*conn)->chan[i] = *chan;
    free(chan);
    chan = NULL;
  }
  if (chan) {
    struct mk_channel_s *c =
      realloc((*conn)->chan, ((*conn)->n + 1) * sizeof(*c));
    if (!c) return mk_e_sys_error("realloc", self);

    (*conn)->chan = c;
    (*conn)->chan[(*conn)->n++] = *chan;
    free(chan);
  }

  /* configure data streaming */
  mk_set_sensor_rate(&sensor_time->rate, *conn, NULL, sensor_time, self);

  return rotorcraft_ether;
}


/* --- Activity disconnect ---------------------------------------------- */

/** Codel mk_disconnect_start of activity disconnect.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_disconnect_start(rotorcraft_conn_s **conn,
                    const genom_context self)
{
  uint32_t i;

  mk_set_sensor_rate(
    &(struct rotorcraft_ids_sensor_time_s_rate_s){
      .imu = 0, .motor = 0, .battery = 0
        }, *conn, NULL, NULL, self);

  for(i = 0; i < (*conn)->n; i++) {
    if ((*conn)->chan[i].fd < 0) continue;

    mk_send_msg(&(*conn)->chan[i], "x");
    close((*conn)->chan[i].fd);
    (*conn)->chan[i].fd = -1;
    warnx("disconnected from %s", (*conn)->chan[i].path);
  }

  return rotorcraft_ether;
}


/* --- Activity monitor ------------------------------------------------- */

/** Codel mk_monitor_check of activity monitor.
 *
 * Triggered by rotorcraft_start, rotorcraft_sleep.
 * Yields to rotorcraft_pause_sleep, rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_monitor_check(const rotorcraft_conn_s *conn,
                 const genom_context self)
{
  (void)self;
  uint32_t i;

  for(i = 0; i < conn->n; i++)
    if (conn->chan[i].fd >= 0) return rotorcraft_pause_sleep;

  return rotorcraft_ether;
}


/* --- mk_connect_chan ----------------------------------------------------- */

genom_event
mk_connect_chan(const char serial[64], uint32_t baud, struct mk_channel_s *chan,
                const genom_context self)
{
  rotorcraft_conn_s conn = { .chan = chan, .n = 1 };
  struct timeval deadline;
  struct stat sb;
  double rev;
  size_t c;
  int s;

  /* open tty */
  chan->fd = mk_open_tty(serial, baud);
  if (chan->fd < 0) return mk_e_sys_error(serial, self);

  if (fstat(chan->fd, &sb)) return mk_e_sys_error(serial, self);
  chan->st_dev = sb.st_dev;
  chan->st_ino = sb.st_ino;
  chan->r = chan->w = 0;
  chan->start = chan->escape = false;

  /* check endpoint */
  while (mk_recv_msg(chan, true) == 1); /* flush buffer */
  do {
    c = 0;
    do {
      if (mk_send_msg(chan, "?")) /* ask for id */
        return mk_e_sys_error(serial, self);

      /* 500ms timeout */
      gettimeofday(&deadline, NULL);
      deadline.tv_usec += 500000; /* exceeding 1e6 is not an issue */
      do {
        s = mk_wait_msg(&conn, &deadline);
      } while(s < 0 && errno == EINTR);

      if (s < 0) return mk_e_sys_error(serial, self);
      if (s > 0) break;
    } while(c++ < 3);
    if (c > 3) {
      warnx("no response from %s", serial);
      errno = ETIMEDOUT;
      return mk_e_sys_error(NULL, self);
    }

    s = mk_recv_msg(chan, true);
  } while(s == 1 && chan->msg[0] != '?');
  if (s != 1) {
    errno = ENOMSG;
    return mk_e_sys_error(NULL, self);
  }

  /* match device */
  chan->msg[chan->len] = 0;
  chan->device = RC_NONE;
  for (c = 0; c < sizeof(rc_devices)/sizeof(rc_devices[0]); c++) {
    if (!rc_devices[c].match) continue;

    if (sscanf((char *)&chan->msg[1], rc_devices[c].match, &rev) != 1)
      continue;
    if (rev < rc_devices[c].rev) {
      rotorcraft_e_baddev_detail d;
      snprintf(d.dev, sizeof(d.dev), "hardware device version `%g' too old, "
               "version `%g' or newer is required", rev, rc_devices[c].rev);
      close(chan->fd);
      chan->fd = -1;
      return rotorcraft_e_baddev(&d, self);
    }

    chan->device = c;
    break;
  }
  if (chan->device == RC_NONE) {
    rotorcraft_e_baddev_detail d;
    snprintf(d.dev, sizeof(d.dev), "unsupported hardware device `%s'",
             &chan->msg[1]);
    close(chan->fd);
    chan->fd = -1;
    return rotorcraft_e_baddev(&d, self);
  }

  snprintf(chan->path, sizeof(chan->path), "%s", serial);
  warnx("connected to %s, %s", &chan->msg[1], chan->path);

  return genom_ok;
}


/* --- rc_filter_imu_data -------------------------------------------------- */

/* Appy calibration and filter to gyro, accelerometer or magnetometer data */

static void
rc_filter_imu_data(const double raw[3],
                   const double scale[3*3], const double bias[3],
                   double in[3], const double alpha[3], double out[3])
{
  double v[3];
  int i;

  for(i = 0; i < 3; i++)
    v[i] = raw[i] + bias[i];

  for(i = 0; i < 3; i++) {
    in[i] =
      scale[3*i + 0] * v[0] +
      scale[3*i + 1] * v[1] +
      scale[3*i + 2] * v[2];

    if (!isnan(out[i]))
      out[i] += alpha[i] * (in[i] - out[i]);
    else
      out[i] = in[i];
  }
}


/* --- mk_get_ts ----------------------------------------------------------- */

/** Implements Olson, Edwin. "A passive solution to the sensor synchronization
 * problem." International conference on Intelligent Robots and Systems (IROS),
 * 2010 IEEE/RSJ */
static void
mk_get_ts(uint8_t seq, struct timeval atv, double rate,
          rotorcraft_ids_sensor_time_s_ts_s *timings, or_time_ts *ts,
          double *lprate)
{
  static const uint32_t tsshift = 1000000000;

  double ats, df;
  uint8_t ds;

  /* arrival timestamp - offset for better floating point precision */
  ats = (atv.tv_sec - tsshift) + atv.tv_usec * 1e-6;

  /* update estimated rate */
  df = 1. / (ats - timings->last);

  if (df > timings->rmed)
    timings->rerr = (3 * timings->rerr + 1.) / 4.;
  else
    timings->rerr = (3 * timings->rerr - 1.) / 4.;

  if (fabs(timings->rerr) > 0.75)
    timings->rgain *= 2;
  else
    timings->rgain /= 2;
  if (timings->rgain < 0.01) timings->rgain = 0.01;

  if (df > timings->rmed)
    timings->rmed += timings->rgain;
  else
    timings->rmed -= timings->rgain;

  *lprate += 0.1 * (timings->rmed - *lprate);

  /* delta samples */
  ds = seq - timings->seq;
  if (ds > 16)
    /* if too many samples were lost, we might have missed more than 255
     * samples: reset the offset */
    timings->offset = -DBL_MAX;
  else if (rate > 0.1)
    /* consider a 0.1% clock drift on the sender side (for rates >0.1Hz) */
    timings->offset -= 0.001 * ds / rate;
  else
    timings->offset = 0.;

  /* update remote timestamp */
  timings->last = ats;
  timings->seq = seq;
  if (rate > 0.1)
    timings->ts += ds / rate;
  else
    /* for tiny rates, just use arrival timestamp */
    timings->ts = ats;

  /* update offset */
  if (timings->ts - ats > timings->offset)
    timings->offset = timings->ts - ats;

  /* local timestamp - reset offset if it diverged more than 5ms from realtime,
   * maybe the sensor is not sending at the specified rate */
  if (ats - (timings->ts - timings->offset) > 0.005)
    timings->offset = timings->ts - ats;
  else
    ats = timings->ts - timings->offset;

  /* update timestamp */
  ts->sec = floor(ats);
  ts->nsec = (ats - ts->sec) * 1e9;
  ts->sec += tsshift;
}
