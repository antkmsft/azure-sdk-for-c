// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/core/az_mqtt5.h>
#include <azure/core/az_mqtt5_rpc_server.h>
#include <azure/core/az_platform.h>
#include <azure/core/az_result.h>
#include <azure/core/internal/az_log_internal.h>
#include <stdio.h>
#include <stdlib.h>

#include "mqtt_protocol.h"

#include <azure/core/_az_cfg.h>

static az_result root(az_event_policy* me, az_event event);
static az_result waiting(az_event_policy* me, az_event event);
static az_result faulted(az_event_policy* me, az_event event);

AZ_NODISCARD az_result _az_rpc_server_policy_init(
    _az_hfsm* hfsm,
    _az_event_client* event_client,
    az_mqtt5_connection* connection);

static az_event_policy_handler _get_parent(az_event_policy_handler child_state)
{
  az_event_policy_handler parent_state;

  if (child_state == root)
  {
    parent_state = NULL;
  }
  else if (child_state == waiting || child_state == faulted)
  {
    parent_state = root;
  }
  else
  {
    // Unknown state.
    az_platform_critical_error();
    parent_state = NULL;
  }

  return parent_state;
}

static az_result root(az_event_policy* me, az_event event)
{
  az_result ret = AZ_OK;
  (void)me;

  if (_az_LOG_SHOULD_WRITE(event.type))
  {
    _az_LOG_WRITE(event.type, AZ_SPAN_FROM_STR("az_rpc_server"));
  }

  switch (event.type)
  {
    case AZ_HFSM_EVENT_ENTRY:
      break;

    case AZ_HFSM_EVENT_ERROR:
    {
      if (az_result_failed(az_event_policy_send_inbound_event(me, event)))
      {
        az_platform_critical_error();
      }
      break;
    }

    case AZ_HFSM_EVENT_EXIT:
    {
      if (_az_LOG_SHOULD_WRITE(AZ_HFSM_EVENT_EXIT))
      {
        _az_LOG_WRITE(AZ_HFSM_EVENT_EXIT, AZ_SPAN_FROM_STR("az_mqtt5_rpc_server: PANIC!"));
      }

      az_platform_critical_error();
      break;
    }

    case AZ_MQTT5_EVENT_PUBACK_RSP:
    case AZ_EVENT_MQTT5_CONNECTION_OPEN_REQ:
    case AZ_MQTT5_EVENT_CONNECT_RSP:
    case AZ_EVENT_MQTT5_CONNECTION_CLOSE_REQ:
    case AZ_MQTT5_EVENT_DISCONNECT_RSP:
      break;

    default:
      // TODO
      ret = AZ_HFSM_RETURN_HANDLE_BY_SUPERSTATE;
      break;
  }

  return ret;
}

/**
 * @brief start subscription timer
 */
AZ_INLINE az_result _rpc_start_timer(az_mqtt5_rpc_server* me)
{
  _az_event_pipeline* pipeline = &me->_internal.connection->_internal.event_pipeline;
  _az_event_pipeline_timer* timer = &me->_internal.rpc_server_timer;

  _az_RETURN_IF_FAILED(_az_event_pipeline_timer_create(pipeline, timer));

  int32_t delay_milliseconds = (int32_t)me->_internal.options.subscribe_timeout_in_seconds * 1000;

  _az_RETURN_IF_FAILED(az_platform_timer_start(&timer->platform_timer, delay_milliseconds));

  return AZ_OK;
}

/**
 * @brief stop subscription timer
 */
AZ_INLINE az_result _rpc_stop_timer(az_mqtt5_rpc_server* me)
{
  _az_event_pipeline_timer* timer = &me->_internal.rpc_server_timer;
  return az_platform_timer_destroy(&timer->platform_timer);
}

/**
 * @brief helper function to check if an az_span topic matches an az_span subscription
 */
AZ_NODISCARD AZ_INLINE bool az_span_topic_matches_sub(az_span sub, az_span topic)
{
  bool ret;
  // TODO: have this not be mosquitto specific
  if (MOSQ_ERR_SUCCESS
      != mosquitto_topic_matches_sub((char*)az_span_ptr(sub), (char*)az_span_ptr(topic), &ret))
  {
    ret = false;
  }
  return ret;
}

/**
 * @brief Build the reponse payload given the execution finish data
 *
 * @param me
 * @param event_data execution finish data
 *    contains status code, and error message or response payload
 * @param out_data event data for response publish
 * @return az_result
 */
AZ_INLINE az_result _build_response(
    az_mqtt5_rpc_server* me,
    az_mqtt5_rpc_server_execution_rsp_event_data* event_data,
    az_mqtt5_pub_data* out_data)
{
  az_mqtt5_rpc_server* this_policy = (az_mqtt5_rpc_server*)me;

  // if the status indicates failure, add the status message to the user properties
  if (event_data->status < 200 || event_data->status >= 300)
  {
    // TODO: is an error message required on failure?
    _az_PRECONDITION_VALID_SPAN(event_data->error_message, 0, true);
    az_mqtt5_property_stringpair status_message_property
        = { .key = AZ_SPAN_FROM_STR("statusMessage"), .value = event_data->error_message };

    _az_RETURN_IF_FAILED(az_mqtt5_property_bag_stringpair_append(
        &this_policy->_internal.property_bag,
        AZ_MQTT5_PROPERTY_TYPE_USER_PROPERTY,
        &status_message_property));
    out_data->payload = AZ_SPAN_EMPTY;
  }
  // if the status indicates success, add the response payload to the publish and set the content
  // type property
  else
  {
    // TODO: is a payload required?
    _az_PRECONDITION_VALID_SPAN(event_data->response, 0, true);
    az_mqtt5_property_string content_type = { .str = event_data->content_type };

    _az_RETURN_IF_FAILED(az_mqtt5_property_bag_string_append(
        &this_policy->_internal.property_bag, AZ_MQTT5_PROPERTY_TYPE_CONTENT_TYPE, &content_type));

    out_data->payload = event_data->response;
  }

  // Set the status user property
  char status_str[5];
  sprintf(status_str, "%d", event_data->status);
  az_mqtt5_property_stringpair status_property
      = { .key = AZ_SPAN_FROM_STR("status"), .value = az_span_create_from_str(status_str) };

  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_stringpair_append(
      &this_policy->_internal.property_bag,
      AZ_MQTT5_PROPERTY_TYPE_USER_PROPERTY,
      &status_property));

  // Set the correlation data property
  _az_PRECONDITION_VALID_SPAN(event_data->correlation_id, 0, true);
  az_mqtt5_property_binarydata correlation_data = { .bindata = event_data->correlation_id };
  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_binary_append(
      &this_policy->_internal.property_bag,
      AZ_MQTT5_PROPERTY_TYPE_CORRELATION_DATA,
      &correlation_data));

  out_data->properties = &this_policy->_internal.property_bag;
  // use the received response topic as the topic
  out_data->topic = event_data->response_topic;
  out_data->qos = this_policy->_internal.options.response_qos;

  return AZ_OK;
}

/**
 * @brief Handle an incoming request
 *
 * @param this_policy
 * @param data event data received from the publish
 *
 * @return az_result
 */
AZ_INLINE az_result _handle_request(az_mqtt5_rpc_server* this_policy, az_mqtt5_recv_data* data)
{
  _az_PRECONDITION_NOT_NULL(data->properties);
  _az_PRECONDITION_NOT_NULL(this_policy);

  // save the response topic
  az_mqtt5_property_string response_topic;
  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_string_read(
      data->properties, AZ_MQTT5_PROPERTY_TYPE_RESPONSE_TOPIC, &response_topic));

  // save the correlation data to send back with the response
  az_mqtt5_property_binarydata correlation_data;
  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_binarydata_read(
      data->properties, AZ_MQTT5_PROPERTY_TYPE_CORRELATION_DATA, &correlation_data));

  // validate request isn't expired?

  // read the content type so the application can properly deserialize the request
  az_mqtt5_property_string content_type;
  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_string_read(
      data->properties, AZ_MQTT5_PROPERTY_TYPE_CONTENT_TYPE, &content_type));

  az_mqtt5_rpc_server_execution_req_event_data command_data
      = (az_mqtt5_rpc_server_execution_req_event_data){
          .correlation_id = az_mqtt5_property_binarydata_get(&correlation_data),
          .response_topic = az_mqtt5_property_string_get(&response_topic),
          .request_data = data->payload,
          .request_topic = data->topic,
          .content_type = az_mqtt5_property_string_get(&content_type)
        };

  // send to application for execution
  // if ((az_event_policy*)this_policy->inbound_policy != NULL)
  // {
  // az_event_policy_send_inbound_event((az_event_policy*)this_policy, (az_event){.type =
  // AZ_EVENT_RPC_SERVER_EXECUTE_COMMAND_REQ, .data = data});
  // }
  _az_RETURN_IF_FAILED(_az_mqtt5_connection_api_callback(
      this_policy->_internal.connection,
      (az_event){ .type = AZ_EVENT_RPC_SERVER_EXECUTE_COMMAND_REQ, .data = &command_data }));

  az_mqtt5_property_string_free(&content_type);
  az_mqtt5_property_binarydata_free(&correlation_data);
  az_mqtt5_property_string_free(&response_topic);

  return AZ_OK;
}

/**
 * @brief Send a response publish and clear the pending command
 *
 * @param me
 * @param data event data for a publish request
 *
 */
AZ_INLINE az_result _send_response_pub(az_mqtt5_rpc_server* me, az_mqtt5_pub_data data)
{
  az_result ret = AZ_OK;
  // send publish
  ret = az_event_policy_send_outbound_event(
      (az_event_policy*)me, (az_event){ .type = AZ_MQTT5_EVENT_PUB_REQ, .data = &data });

  // empty the property bag so it can be reused
  _az_RETURN_IF_FAILED(az_mqtt5_property_bag_empty(&me->_internal.property_bag));
  return ret;
}

/**
 * @brief Main state where the rpc server waits for incoming command requests or execution to
 * complete
 */
static az_result waiting(az_event_policy* me, az_event event)
{
  az_result ret = AZ_OK;
  az_mqtt5_rpc_server* this_policy = (az_mqtt5_rpc_server*)me;

  if (_az_LOG_SHOULD_WRITE(event.type))
  {
    _az_LOG_WRITE(event.type, AZ_SPAN_FROM_STR("az_rpc_server/waiting"));
  }

  switch (event.type)
  {
    case AZ_HFSM_EVENT_ENTRY:
      // No-op
      break;

    case AZ_MQTT5_EVENT_SUBACK_RSP:
    {
      // if get suback that matches the sub we sent, stop waiting for the suback
      az_mqtt5_suback_data* data = (az_mqtt5_suback_data*)event.data;
      if (data->id == this_policy->_internal.pending_subscription_id)
      {
        _rpc_stop_timer(this_policy);
        this_policy->_internal.pending_subscription_id = 0;
      }
      // else, keep waiting for the proper suback
      break;
    }

    case AZ_HFSM_EVENT_TIMEOUT:
    {
      if (event.data == &this_policy->_internal.rpc_server_timer)
      {
        // if subscribing times out, go to faulted state - this is not recoverable
        _az_RETURN_IF_FAILED(_az_hfsm_transition_peer((_az_hfsm*)me, waiting, faulted));
      }
      break;
    }

    case AZ_MQTT5_EVENT_PUB_RECV_IND:
    {
      az_mqtt5_recv_data* recv_data = (az_mqtt5_recv_data*)event.data;
      // Ensure pub is of the right topic
      if (az_span_topic_matches_sub(this_policy->_internal.subscription_topic, recv_data->topic))
      {
        // clear subscription timer if we get a pub on the topic, since that implies we're
        // subscribed
        if (this_policy->_internal.pending_subscription_id != 0)
        {
          _rpc_stop_timer(this_policy);
          this_policy->_internal.pending_subscription_id = 0;
        }

        // parse the request details and send it to the application for execution
        _az_RETURN_IF_FAILED(_handle_request(this_policy, recv_data));
      }
      break;
    }

    case AZ_EVENT_RPC_SERVER_EXECUTE_COMMAND_RSP:
    {
      az_mqtt5_rpc_server_execution_rsp_event_data* event_data
          = (az_mqtt5_rpc_server_execution_rsp_event_data*)event.data;

      // Check that original request topic matches the subscription topic for this RPC server
      // instance
      if (az_span_topic_matches_sub(
              this_policy->_internal.subscription_topic, event_data->request_topic))
      {
        // create response payload
        az_mqtt5_pub_data data;
        _az_RETURN_IF_FAILED(_build_response(this_policy, event_data, &data));

        // send publish
        _send_response_pub(this_policy, data);
      }
      else
      {
        // log and ignore (this is probably meant for a different policy)
        printf("topic does not match subscription, ignoring\n");
      }
      break;
    }

    case AZ_MQTT5_EVENT_PUBACK_RSP:
    case AZ_EVENT_MQTT5_CONNECTION_OPEN_REQ:
    case AZ_MQTT5_EVENT_CONNECT_RSP:
      break;

    case AZ_HFSM_EVENT_EXIT:
      // No-op
      break;

    default:
      // TODO
      ret = AZ_HFSM_RETURN_HANDLE_BY_SUPERSTATE;
      break;
  }

  return ret;
}

/**
 * @brief Failure state - locks up all execution of this hfsm
 */
static az_result faulted(az_event_policy* me, az_event event)
{
  az_result ret = AZ_ERROR_HFSM_INVALID_STATE;
  (void)me;
#ifdef AZ_NO_LOGGING
  (void)event;
#endif // AZ_NO_LOGGING

  if (_az_LOG_SHOULD_WRITE(event.type))
  {
    _az_LOG_WRITE(event.type, AZ_SPAN_FROM_STR("az_rpc_server/faulted"));
  }

  return ret;
}

AZ_NODISCARD az_result _az_rpc_server_policy_init(
    _az_hfsm* hfsm,
    _az_event_client* event_client,
    az_mqtt5_connection* connection)
{
  _az_RETURN_IF_FAILED(_az_hfsm_init(hfsm, root, _get_parent, NULL, NULL));
  _az_RETURN_IF_FAILED(_az_hfsm_transition_substate(hfsm, root, waiting));

  event_client->policy = (az_event_policy*)hfsm;
  _az_RETURN_IF_FAILED(_az_event_policy_collection_add_client(
      &connection->_internal.policy_collection, event_client));

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_rpc_server_register(az_mqtt5_rpc_server* client)
{
  if (client->_internal.connection == NULL)
  {
    // This API can be called only when the client is attached to a connection object.
    return AZ_ERROR_NOT_SUPPORTED;
  }

  az_mqtt5_sub_data subscription_data = { .topic_filter = client->_internal.subscription_topic,
                                          .qos = client->_internal.options.subscribe_qos,
                                          .out_id = 0 };
  _rpc_start_timer(client);
  _az_RETURN_IF_FAILED(az_event_policy_send_outbound_event(
      (az_event_policy*)client,
      (az_event){ .type = AZ_MQTT5_EVENT_SUB_REQ, .data = &subscription_data }));
  client->_internal.pending_subscription_id = subscription_data.out_id;
  return AZ_OK;
}

AZ_NODISCARD az_mqtt5_rpc_server_options az_mqtt5_rpc_server_options_default()
{
  return (az_mqtt5_rpc_server_options){ .subscribe_qos = AZ_MQTT5_RPC_QOS,
                                        .response_qos = AZ_MQTT5_RPC_QOS,
                                        .subscribe_timeout_in_seconds
                                        = AZ_MQTT5_RPC_SERVER_DEFAULT_TIMEOUT_SECONDS };
}

AZ_NODISCARD az_result az_rpc_server_init(
    az_mqtt5_rpc_server* client,
    az_mqtt5_connection* connection,
    az_mqtt5_property_bag property_bag,
    az_span subscription_topic,
    az_span model_id,
    az_span client_id,
    az_span command_name,
    az_mqtt5_rpc_server_options* options)
{
  _az_PRECONDITION_NOT_NULL(client);
  client->_internal.options = options == NULL ? az_mqtt5_rpc_server_options_default() : *options;

  // _az_PRECONDITION_NOT_NULL(options->property_bag);
  _az_PRECONDITION_VALID_SPAN(model_id, 1, false);
  _az_PRECONDITION_VALID_SPAN(client_id, 1, false);
#ifndef AZ_NO_PRECONDITION_CHECKING
  int32_t subscription_min_length = az_span_size(model_id) + az_span_size(client_id)
      + (az_span_size(command_name) > 0 ? az_span_size(command_name) : 1) + 23;
  _az_PRECONDITION_VALID_SPAN(subscription_topic, subscription_min_length, true);
#endif

  client->_internal.property_bag = property_bag;

  az_span temp_span = subscription_topic;
  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR("vehicles/"));
  temp_span = az_span_copy(temp_span, model_id);
  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR("/commands/"));
  temp_span = az_span_copy(temp_span, client_id);
  temp_span = az_span_copy_u8(temp_span, '/');
  temp_span = az_span_copy(
      temp_span, _az_span_is_valid(command_name, 1, 0) ? command_name : AZ_SPAN_FROM_STR("+"));
  temp_span = az_span_copy_u8(temp_span, '\0');

  client->_internal.subscription_topic = subscription_topic;

  client->_internal.connection = connection;

  // Initialize the stateful sub-client.
  if ((connection != NULL))
  {
    _az_RETURN_IF_FAILED(
        _az_rpc_server_policy_init((_az_hfsm*)client, &client->_internal.subclient, connection));
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_rpc_server_execution_finish(
    az_mqtt5_rpc_server* client,
    az_mqtt5_rpc_server_execution_rsp_event_data* data)
{
  if (client->_internal.connection == NULL)
  {
    // This API can be called only when the client is attached to a connection object.
    return AZ_ERROR_NOT_SUPPORTED;
  }

  _az_PRECONDITION_VALID_SPAN(data->correlation_id, 1, false);
  _az_PRECONDITION_VALID_SPAN(data->response_topic, 1, false);
  // _az_PRECONDITION_VALID_SPAN(data->response, 0, true);
  // _az_PRECONDITION_VALID_SPAN(data->error_message, 0, true);

  return _az_event_pipeline_post_outbound_event(
      &client->_internal.connection->_internal.event_pipeline,
      (az_event){ .type = AZ_EVENT_RPC_SERVER_EXECUTE_COMMAND_RSP, .data = data });
}