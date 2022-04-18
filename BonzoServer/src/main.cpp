#include <stdio.h>
#include <string>
#include <vector>

#include "mongoose.h"

struct Room {
	std::string StreamingRoomName;
  bool IsOwnerRoom;
};

struct User {
	Room* CurrentRoom;
  Room* OwnerRoom;
	std::string UserName;
  bool IsSuperUser;
};

std::vector<Room*> Rooms;
std::vector<User*> Users;

static sig_atomic_t s_signal_received = 0;
static const char *s_http_port = "8000";
static struct mg_serve_http_opts s_http_server_opts;

void Network_Break_URL(std::string ServerURL, std::string& RoomName, std::string& NickName) {
  
    std::size_t LastPart = ServerURL.rfind('/');
    if(LastPart == std::string::npos || LastPart<=1) {
      RoomName = ServerURL;
    } else {
      RoomName = ServerURL.substr(0,LastPart);
      NickName = ServerURL.substr(LastPart+1);
    }
}

static void signal_handler(int sig_num) {
	signal(sig_num, signal_handler);  // Reinstantiate signal handler
	s_signal_received = sig_num;
}

static int is_websocket(const struct mg_connection *nc) {
	return nc->flags & MG_F_IS_WEBSOCKET;
}

static void broadcast(struct mg_connection *nc, const struct mg_str msg) {
	
	User* CurUser = (User*)nc->user_data;
  
    /*
	char addr[32];
	mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
		MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);

	printf("Message from %s\n", addr);
    */

	printf("%s | Message from %s\n", CurUser->CurrentRoom->StreamingRoomName.c_str(), CurUser->UserName.c_str());

  //char buf[500];
	//snprintf(buf, sizeof(buf), "%s %.*s", addr, (int)msg.len, msg.p);
	//snprintf(buf, sizeof(buf), "*s", msg.p);
	//printf("%s\n", buf);

	struct mg_connection *c;
	for (c = mg_next(nc->mgr, NULL); c != NULL; c = mg_next(nc->mgr, c)) {
		if (c == nc) continue; /* Don't send to the sender. */
		User* TargetUser = (User*)c->user_data;
    // Only send if in the same room
		if (TargetUser) {
      if(TargetUser->IsSuperUser || TargetUser->CurrentRoom == CurUser->CurrentRoom || TargetUser->CurrentRoom == CurUser->OwnerRoom) {
			  mg_send_websocket_frame(c, WEBSOCKET_OP_TEXT, msg.p, msg.len);
      }
	  }
	}
}

Room* GetRoom(std::string roomName, bool IsOwnerRoom) {
  Room* TargetRoom = NULL;
  
  //to lowercase
  for (int i = 0; i < roomName.length(); i++)
  {
    roomName[i] = tolower(roomName[i]);
  }

	for (int i = 0; i < Rooms.size(); ++i) {
		if (Rooms[i]->StreamingRoomName == roomName) {
			//printf("Found room %s\n", joinedRoom.c_str());
			TargetRoom = Rooms[i];
			break;
	  }
	}
	if (!TargetRoom) { // no room found
		Room* NewRoom = new Room();
		NewRoom->StreamingRoomName = roomName;
    NewRoom->IsOwnerRoom = IsOwnerRoom;
		Rooms.push_back(NewRoom);
		TargetRoom = NewRoom;
	}
  return TargetRoom;
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
	switch (ev) {
	case MG_EV_WEBSOCKET_HANDSHAKE_DONE: {
		/* New websocket connection. Tell everybody. */
		//broadcast(nc, mg_mk_str("++ joined"));
		char addr[32];
		mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT | MG_SOCK_STRINGIFY_REMOTE);

		struct http_message *hm = (struct http_message *) ev_data;
    std::string urlParam="";
    urlParam.append(hm->uri.p, hm->uri.len);
		std::string baseRoomName,nickName;
    bool IsSuperUser = false;
    Network_Break_URL(urlParam, baseRoomName, nickName);
    if(baseRoomName.length() == 0 || baseRoomName == "/") {
      baseRoomName = "SuperRoom";
      IsSuperUser = true;
    }
    std::string streamingRoomName = baseRoomName;
    bool IsOwnerRoom = true;
    if(nickName.length() > 0 && nickName != "/") {
      streamingRoomName += "/" + nickName;
      IsOwnerRoom = false;
    }
		
		Room* TargetRoom = GetRoom(streamingRoomName, IsOwnerRoom);
		Room* OwnerRoom = TargetRoom;
    if(!IsOwnerRoom) {
      OwnerRoom = GetRoom(baseRoomName, true);
    }

		User* NewUser = new User();
		NewUser->UserName = IsOwnerRoom ? baseRoomName : nickName;
		NewUser->CurrentRoom = TargetRoom;
    NewUser->OwnerRoom = OwnerRoom;
    NewUser->IsSuperUser = IsSuperUser;
		Users.push_back(NewUser);

		nc->user_data = NewUser;
		printf("%s ++ joined %s\n", addr, streamingRoomName.c_str());
		break;
	}
	case MG_EV_WEBSOCKET_FRAME: {
		struct websocket_message *wm = (struct websocket_message *) ev_data;
		/* New websocket message. Tell everybody. */
		struct mg_str d = { (char *)wm->data, wm->size };
		broadcast(nc, d);
		break;
	}
  case MG_EV_POLL: {

    break;
  }
	case MG_EV_HTTP_REQUEST: {
		mg_serve_http(nc, (struct http_message *) ev_data, s_http_server_opts);
		break;
	}
	case MG_EV_CLOSE: {
		/* Disconnect. Tell everybody. */
		if (is_websocket(nc)) {
			//broadcast(nc, mg_mk_str("-- left"));
			char addr[32];
			mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);

			for (auto it = begin(Users); it != end(Users); ++it) {
				User* Cur = (*it);
				if (Cur == nc->user_data) {
					//printf("Found %s\n", Cur->CurrentRoom->RoomName.c_str());
					Users.erase(it);
					delete Cur;
					break;
		    }
			}

			printf("%s -- left\n", addr);
		}
		break;
	}
	}
}

int main(int argc, const char *argv[])
{
	printf("[SERVER] Started \n");

	struct mg_mgr mgr;
	struct mg_connection *nc;
    
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	//setvbuf(stdout, NULL, _IOLBF, 0);
	//setvbuf(stderr, NULL, _IOLBF, 0);

    
	mg_mgr_init(&mgr, NULL);

	nc = mg_bind(&mgr, s_http_port, ev_handler);
	mg_set_protocol_http_websocket(nc);
	s_http_server_opts.document_root = ".";  // Serve current directory
	s_http_server_opts.enable_directory_listing = "yes";

	printf("Started on port %s\n", s_http_port);
	while (s_signal_received == 0) {
		mg_mgr_poll(&mgr, 200);
	}
	mg_mgr_free(&mgr);
    
  // Clean
	for (auto Cur : Rooms) {
		delete Cur;
	}
	Rooms.clear();

	return 0;
}
