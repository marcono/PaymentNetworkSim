#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_math.h>

#include "../include/htlc.h"
#include "../include/array.h"
#include "../include/heap.h"
#include "../include/payments.h"
#include "../include/routing.h"
#include "../include/network.h"
#include "../include/event.h"


/* AUXILIARY FUNCTIONS */

int is_present(long element, struct array* long_array) {
  long i, *curr;

  if(long_array==NULL) return 0;

  for(i=0; i<array_len(long_array); i++) {
    curr = array_get(long_array, i);
    if(*curr==element) return 1;
  }

  return 0;
}


int is_equal(long* a, long* b) {
  return *a==*b;
}


uint64_t compute_fee(uint64_t amount_to_forward, struct policy policy) {
  uint64_t fee;
  fee = (policy.fee_proportional*amount_to_forward) / 1000000;
  return policy.fee_base + fee;
}


int is_equal_ignored(struct ignored* a, struct ignored* b){
  return a->id == b->id;
}


void check_ignored(struct node* sender, uint64_t current_time){
  struct array* ignored_edges;
  struct ignored* ignored;
  int i;

  ignored_edges = sender->ignored_edges;

  for(i=0; i<array_len(ignored_edges); i++){
    ignored = array_get(ignored_edges, i);

    //register time of newly added ignored edges
    if(ignored->time==0)
      ignored->time = current_time;

    //remove decayed ignored edges
    if(current_time > 5000 + ignored->time){
      array_delete(ignored_edges, ignored, is_equal_ignored);
    }
  }
}


/* int check_policy_forward( struct route_hop* prev_hop, struct route_hop* curr_hop, struct array* edges) { */
/*   struct policy policy; */
/*   struct edge* curr_edge, *prev_edge; */
/*   uint64_t fee; */

/*   curr_edge = array_get(edges, curr_hop->path_hop->edge); */
/*   prev_edge = array_get(edges, prev_hop->path_hop->edge); */

/*   fee = compute_fee(curr_hop->amount_to_forward,curr_edge->policy); */
/*   //the check should be: prev_hop->amount_to_forward - fee != curr_hop->amount_to_forward */
/*   if(prev_hop->amount_to_forward - fee != curr_hop->amount_to_forward) { */
/*     printf("ERROR: Fee not respected\n"); */
/*     printf("Prev_hop_amount %ld - fee %ld != Curr_hop_amount %ld\n", prev_hop->amount_to_forward, fee, curr_hop->amount_to_forward); */
/*     print_hop(curr_hop); */
/*     return 0; */
/*   } */

/*   if(prev_hop->timelock - prev_edge->policy.timelock != curr_hop->timelock) { */
/*     printf("ERROR: Timelock not respected\n"); */
/*     printf("Prev_hop_timelock %d - policy_timelock %d != curr_hop_timelock %d \n",prev_hop->timelock, policy.timelock, curr_hop->timelock); */
/*     print_hop(curr_hop); */
/*     return 0; */
/*   } */

/*   return 1; */
/* } */


void add_ignored_edge(long node_id, long ignored_id, struct array* nodes){
  struct ignored* ignored;
  struct node* node;

  ignored = (struct ignored*)malloc(sizeof(struct ignored));
  ignored->id = ignored_id;
  ignored->time = 0;

  node = array_get(nodes, node_id);
  node->ignored_edges = array_insert(node->ignored_edges, ignored);
}


struct route_hop *get_route_hop(long node_id, struct array *route_hops, int is_sender) {
  struct route_hop *route_hop;
  long i, index = -1;

  for (i = 0; i < array_len(route_hops); i++) {
    route_hop = array_get(route_hops, i);

    if (is_sender && route_hop->path_hop->sender == node_id) {
      index = i;
      break;
    }

    if (!is_sender && route_hop->path_hop->receiver == node_id) {
      index = i;
      break;
    }
  }

  if (index == -1)
    return NULL;

  return array_get(route_hops, index);
}


/*HTLC FUNCTIONS*/


void find_route(struct event *event, struct simulation* simulation, struct network* network) {
  struct payment *payment;
  struct node* node;
  struct array *path_hops;
  struct route* route;
  struct event* send_event;
  uint64_t next_event_time;
  enum pathfind_error error;


  payment = event->payment;
  node = array_get(network->nodes, payment->sender);

  printf("FINDROUTE %ld\n", payment->id);

  ++(payment->attempts);


  if(simulation->current_time > payment->start_time + 60000) {
    payment->end_time = simulation->current_time;
    payment->is_timeout = 1;
    return;
  }

  //  sender = array_get(network->nodes, payment->sender);
  check_ignored(node, simulation->current_time);


  if (payment->attempts==1)
    path_hops = paths[payment->id];
  else
    path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, network, 0, &error);


  if (path_hops == NULL) {
    printf("No available path\n");
    payment->end_time = simulation->current_time;
    return;
  }

  route = transform_path_into_route(path_hops, payment->amount, network);
  if(route==NULL) {
    printf("No available route\n");
    payment->end_time = simulation->current_time;
    return;
  }

  payment->route = route;

  next_event_time = simulation->current_time;
  send_event = new_event(next_event_time, SENDPAYMENT, payment->sender, event->payment );
  simulation->events = heap_insert(simulation->events, send_event, compare_event);

}



void send_payment(struct event* event, struct simulation* simulation, struct network *network) {
  struct payment* payment;
  uint64_t next_event_time;
  struct route* route;
  struct route_hop* first_route_hop;
  struct edge* next_edge;
  struct event* next_event;
  enum event_type event_type;
  struct node* node;


  payment = event->payment;
  node = array_get(network->nodes, event->node_id);
  route = payment->route;
  first_route_hop = array_get(route->route_hops, 0);
  next_edge = array_get(network->edges, first_route_hop->path_hop->edge);

  if(!is_present(next_edge->id, node->open_edges)) {
    printf("struct edge %ld has been closed\n", next_edge->id);
    next_event_time = simulation->current_time;
    next_event = new_event(next_event_time, FINDROUTE, event->node_id, event->payment );
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  next_edge->tot_flows += 1;

  if(first_route_hop->amount_to_forward > next_edge->balance) {
    printf("Not enough balance in edge %ld\n", next_edge->id);
    add_ignored_edge(payment->sender, next_edge->id, network->nodes);
    next_event_time = simulation->current_time;
    next_event = new_event(next_event_time, FINDROUTE, event->node_id, event->payment );
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  next_edge->balance -= first_route_hop->amount_to_forward;



  event_type = first_route_hop->path_hop->receiver == payment->receiver ? RECEIVEPAYMENT : FORWARDPAYMENT;
  next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);
  next_event = new_event(next_event_time, event_type, first_route_hop->path_hop->receiver, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

void forward_payment(struct event *event, struct simulation* simulation, struct network* network) {
  struct payment* payment;
  struct route* route;
  struct route_hop* next_route_hop, *previous_route_hop;
  long  next_node_id, prev_node_id;
  enum event_type event_type;
  struct event* next_event;
  uint64_t next_event_time;
  struct edge* prev_edge, *next_edge;
  int is_faulty;
  struct node* node;

  payment = event->payment;
  node = array_get(network->nodes, event->node_id);
  route = payment->route;
  next_route_hop=get_route_hop(node->id, route->route_hops, 1);
  previous_route_hop = get_route_hop(node->id, route->route_hops, 0);
  prev_edge = array_get(network->edges, previous_route_hop->path_hop->edge);
  next_edge = array_get(network->edges, next_route_hop->path_hop->edge);

  if(next_route_hop == NULL || previous_route_hop == NULL) {
    printf("ERROR: no route hop\n");
    return;
  }

  if(!is_present(next_route_hop->path_hop->edge, node->open_edges)) {
    printf("struct edge %ld has been closed\n", next_route_hop->path_hop->edge);
    prev_node_id = previous_route_hop->path_hop->sender;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//prev_channel->latency;
    next_event = new_event( next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  is_faulty = gsl_ran_discrete(simulation->random_generator, network->faulty_node_prob);
  if(is_faulty){
    printf("node %ld is faulty\n", event->node_id);
    payment->uncoop_before = 1;
    add_ignored_edge(payment->sender, prev_edge->id, network->nodes);

    prev_node_id = previous_route_hop->path_hop->sender;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator) + FAULTYLATENCY;//prev_channel->latency + FAULTYLATENCY;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }


  /* if(!is_cooperative_after_lock()) { */
  /*   printf("struct peer %ld is not cooperative after lock on channel %ld\n", event->peer_id, prev_channel->id); */
  /*   payment->uncoop_after = 1; */
  /*   close_channel(prev_channel->channel_info_id); */

  /*   payment->is_success = 0; */
  /*   payment->end_time = simulation->current_time; */
  /*   return; */
  /* } */



  /* is_policy_respected = check_policy_forward(previous_route_hop, next_route_hop, network->edges); */
  /* if(!is_policy_respected) return; */

  next_edge->tot_flows += 1;

  if(next_route_hop->amount_to_forward > next_edge->balance ) {
    printf("Not enough balance in edge %ld\n", next_edge->id);
    add_ignored_edge(payment->sender, next_edge->id, network->nodes);

    prev_node_id = previous_route_hop->path_hop->sender;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//prev_channel->latency;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }
  next_edge->balance -= next_route_hop->amount_to_forward;



  next_node_id = next_route_hop->path_hop->receiver;
  event_type = next_node_id == payment->receiver ? RECEIVEPAYMENT : FORWARDPAYMENT;
  next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//next_channel->latency;
  next_event = new_event(next_event_time, event_type, next_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);

}


void receive_payment(struct event* event, struct simulation* simulation, struct network* network) {
  long  prev_node_id;
  struct route* route;
  struct payment* payment;
  struct route_hop* last_route_hop;
  struct edge* forward_edge,*backward_edge;
  struct event* next_event;
  enum event_type event_type;
  uint64_t next_event_time;
  int is_faulty;

  payment = event->payment;
  route = payment->route;

  last_route_hop = array_get(route->route_hops, array_len(route->route_hops) - 1);
  forward_edge = array_get(network->edges, last_route_hop->path_hop->edge);
  backward_edge = array_get(network->edges, forward_edge->counter_edge_id);

  backward_edge->balance += last_route_hop->amount_to_forward;

  is_faulty = gsl_ran_discrete(simulation->random_generator, network->faulty_node_prob); 
  if(is_faulty){
    printf("node %ld is faulty\n", event->node_id);
    payment->uncoop_before = 1;
    add_ignored_edge(payment->sender, forward_edge->id, network->nodes);

    prev_node_id = last_route_hop->path_hop->sender;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator) + FAULTYLATENCY;//channel->latency + FAULTYLATENCY;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment );
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }

  payment->is_success = 1;

  prev_node_id = last_route_hop->path_hop->sender;
  event_type = prev_node_id == payment->sender ? RECEIVESUCCESS : FORWARDSUCCESS;
  next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}


void forward_success(struct event* event, struct simulation* simulation, struct network* network) {
  struct route_hop* prev_hop;
  struct payment* payment;
  struct edge* forward_edge, * backward_edge;
  long prev_node_id;
  struct event* next_event;
  enum event_type event_type;
  struct node* node;
  uint64_t next_event_time;


  payment = event->payment;
  prev_hop = get_route_hop(event->node_id, payment->route->route_hops, 0);
  forward_edge = array_get(network->edges, prev_hop->path_hop->edge);
  backward_edge = array_get(network->edges, forward_edge->counter_edge_id);
  node = array_get(network->nodes, event->node_id);

  if(!is_present(backward_edge->id, node->open_edges)) {
    printf("struct edge %ld is not present\n", prev_hop->path_hop->edge);
    prev_node_id = prev_hop->path_hop->sender;
    event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
    next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//prev_channel->latency;
    next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
    simulation->events = heap_insert(simulation->events, next_event, compare_event);
    return;
  }



  /* if(!is_cooperative_after_lock()) { */
  /*   printf("struct node %ld is not cooperative after lock on edge %ld\n", event->node_id, next_edge->id); */
  /*   payment->uncoop_after = 1; */
  /*   close_channel(next_edge->channel_id); */

  /*   payment->is_success = 0; */
  /*   payment->end_time = simulation->current_time; */

  /*   return; */
  /* } */



  backward_edge->balance += prev_hop->amount_to_forward;


  prev_node_id = prev_hop->path_hop->sender;
  event_type = prev_node_id == payment->sender ? RECEIVESUCCESS : FORWARDSUCCESS;
  next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//prev_channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}

void receive_success(struct event* event, struct simulation *simulation){
  printf("RECEIVE SUCCESS %ld\n", event->payment->id);
  event->payment->end_time = simulation->current_time;
}


void forward_fail(struct event* event, struct simulation* simulation, struct network* network) {
  struct payment* payment;
  struct route_hop* next_hop, *prev_hop;
  struct edge* next_edge;
  long prev_node_id;
  struct event* next_event;
  enum event_type event_type;
  struct node* node;
  uint64_t next_event_time;

  node = array_get(network->nodes, event->node_id);
  payment = event->payment;
  next_hop = get_route_hop(event->node_id, payment->route->route_hops, 1);
  next_edge = array_get(network->edges, next_hop->path_hop->edge);

  if(is_present(next_edge->id, node->open_edges)) {
    next_edge->balance += next_hop->amount_to_forward;
  }
  else
    printf("struct edge %ld is not present\n", next_hop->path_hop->edge);

  prev_hop = get_route_hop(event->node_id, payment->route->route_hops, 0);
  prev_node_id = prev_hop->path_hop->sender;
  event_type = prev_node_id == payment->sender ? RECEIVEFAIL : FORWARDFAIL;
  next_event_time = simulation->current_time + 100 + gsl_ran_ugaussian(simulation->random_generator);//prev_channel->latency;
  next_event = new_event(next_event_time, event_type, prev_node_id, event->payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}


void receive_fail(struct event* event, struct simulation* simulation, struct network* network) {
  struct payment* payment;
  struct route_hop* first_hop;
  struct edge* next_edge;
  struct event* next_event;
  struct node* node;
  uint64_t next_event_time;

  payment = event->payment;

  printf("RECEIVE FAIL %ld\n", payment->id);

  first_hop = array_get(payment->route->route_hops, 0);
  next_edge = array_get(network->edges, first_hop->path_hop->edge);
  node = array_get(network->nodes, event->node_id);

  if(is_present(next_edge->id, node->open_edges))
    next_edge->balance += first_hop->amount_to_forward;
  else
    printf("struct edge %ld is not present\n", next_edge->id);

  if(payment->is_success == 1 ) {
    payment->end_time = simulation->current_time;
    return; //it means that money actually arrived to the destination but a node was not cooperative when forwarding the success
  }

  next_event_time = simulation->current_time;
  next_event = new_event(next_event_time, FINDROUTE, payment->sender, payment);
  simulation->events = heap_insert(simulation->events, next_event, compare_event);
}





//FUNCTIONS FOR UNCOOPERATIVE-AFTER-LOCK BEHAVIOR

/* void close_channel(long channel_id) { */
/*   long i; */
/*   struct node *node; */
/*   struct channel *channel; */
/*   struct edge* direction1, *direction2; */

/*   channel = array_get(channels, channel_id); */
/*   direction1 = array_get(edges, channel->edge1); */
/*   direction2 = array_get(edges, channel->edge2); */

/*   channel->is_closed = 1; */
/*   direction1->is_closed = 1; */
/*   direction2->is_closed = 1; */

/*   printf("struct channel %ld, struct edge_direction1 %ld, struct edge_direction2 %ld are now closed\n", channel->id, channel->edge1, channel->edge2); */

/*   for(i = 0; i < node_index; i++) { */
/*     node = array_get(nodes, i); */
/*     array_delete(node->open_edges, &(channel->edge1), is_equal); */
/*     array_delete(node->open_edges, &(channel->edge2), is_equal); */
/*   } */
/* } */

/* int is_cooperative_after_lock() { */
/*   return gsl_ran_discrete(random_generator, uncoop_after_discrete); */
/* } */

/* unsigned int is_any_channel_closed(struct array* hops) { */
/*   int i; */
/*   struct edge* edge; */
/*   struct path_hop* hop; */

/*   for(i=0;i<array_len(hops);i++) { */
/*     hop = array_get(hops, i); */
/*     edge = array_get(edges, hop->edge); */
/*     if(edge->is_closed) */
/*       return 1; */
/*   } */

/*   return 0; */
/* } */


//old versions for route finding (to be placed in find_route())
  /*
  // dijkstra version
  path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, payment->ignored_nodes,
                      payment->ignored_edges);
*/  

  /* floyd_warshall version
  if(payment->attempts == 0) {
    path_hops = get_path(payment->sender, payment->receiver);
  }
  else {
    path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, payment->ignored_nodes,
                        payment->ignored_edges);
                        }*/

  //dijkstra parallel OLD OLD version
  /* if(payment->attempts > 0) */
  /*   pthread_create(&tid, NULL, &dijkstra_thread, payment); */

  /* pthread_mutex_lock(&(cond_mutex[payment->id])); */
  /* while(!cond_paths[payment->id]) */
  /*   pthread_cond_wait(&(cond_var[payment->id]), &(cond_mutex[payment->id])); */
  /* cond_paths[payment->id] = 0; */
  /* pthread_mutex_unlock(&(cond_mutex[payment->id])); */


  //dijkstra parallel OLD version
  /* if(payment->attempts==1) { */
  /*   path_hops = paths[payment->id]; */
  /*   if(path_hops!=NULL) */
  /*     if(is_any_channel_closed(path_hops)) { */
  /*       path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, node->ignored_nodes, */
  /*                            node->ignored_edges, network, 0); */
  /*     } */
  /* } */
  /* else { */
  /*   path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, node->ignored_nodes, */
  /*                        node->ignored_edges, network, 0); */
  /* } */

  /* if(path_hops!=NULL) */
  /*   if(is_any_channel_closed(path_hops)) { */
  /*     path_hops = dijkstra(payment->sender, payment->receiver, payment->amount, node->ignored_nodes, */
  /*                          node->ignored_edges, network, 0); */
  /*     paths[payment->id] = path_hops; */
  /*   } */
