#pragma once

// TesserAPI: describe firmware state once, serve it as a queryable JSON tree.
// See docs/DESIGN.md.

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/client.h"
#include "tesser/datagram.h"
#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/line_transport.h"
#include "tesser/remote.h"
#include "tesser/storage.h"
#include "tesser/stream_subscriber.h"
#include "tesser/udp_link.h"
#include "tesser/transports/http_server.h"
#include "tesser/transports/nowtp_transport.h"
#include "tesser/transports/stream_transport.h"
#include "tesser/transports/uart_transport.h"
#include "tesser/transports/udp_transport.h"
