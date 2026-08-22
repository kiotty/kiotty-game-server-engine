# Core/

1. kiotty_stream.h 변경

- ISink<T> 만들기
    - virtual bool emit(const T& item) = 0;

- struct StreamListener<T>
    - virtual void onStream(T& value) {};

- IStream<T>
    - virtual void addOnStream(StreamListener<T> onStream) = 0;
    - virtual void clear() = 0;

- IMutableStream<T>
    - virtual ISink<T>& sink() = 0;
    - virtual IStream<T>& stream() = 0;

- MutableStream<T> : public IMutableStream<T>
    - bool emit(void* user_context, const T& item) override;
    - void addOnStream(OnData<T> onData) override;
    - ISink<T>& sink();
    - IStream<T>& stream();
    
- MutableStream은 최대한 힙 할당 없게 구현.
    - 힙 할당을 없애기 위하여 std::function 대신 Raw Function Pointer를 사용함.
    - 더 Modern한 방법 있으면 추천 부탁해

# Domain/

1. entity/에 아래의 entity들 생성


- GameRequest // Sink 데이터 없음
    - 필요한 데이터들   
- GameResponse 
    - 필요한 데이터들
- GameEvent // Response와 거의 동일함.

- ConnectionInfo
    - ip
    - port


2. channel/에 presentation과 business끼리 서로 데이터 교환용 channel 만들기

- GameChannel
    - IoGameChannel io();
    - BusinessGameChannel buisness();
    - private member
        - size_t channel_id;
        - MutableStream<GameRequest>
        - MutableStream<GameResponse>
        - MutableStream<GameEvent>

- IoGameChannel
    - size_t channel_id;
    - ISink<GameRequest>& request;
    - IStream<GameResponse>& response;
    - IStream<GameEvent>& event;

- BusinessGameChannel
    - size_t channel_id;
    - IStream<GameRequest>& request;
    - ISink<GameResponse>& response;
    - ISink<GameEvent>& event;

- GameChannelPool
    - GameChannel& create();
    - void remove(GameChannel& connection);
    - GameChannel& find(size_t channel_id);
    // 1) Event 보내기
    ```cpp
        worker.run(task);
            // game event가 생성
            Event event = somerepository.createEvent();

            // 관련된 session들 생성
            GameSession related_sessions[MAX_GAME_SESSION];
            somerepository.findSessions(related_sessions);

            // session과 연관된 connection 찾음. session이 자기와 연결된 conneciton index 정보를 저장하고 있음.
            for (auto& session : related_sessions)
            {
                auto& channel = game_channel_pool.find(session.channel_idx);
                channel.business().event.emit(event);
            }
            
    ```

- ConnectionHandler // 더 적절한 이름 있으면 추천해줘
    - virtual IoGameChannel onConnected(const ConnectionInfo& info) = 0;
    - virtual void *onDisconnected(const ConnectionInfo& info, IoGameChannel& requester) = 0;
    // 여러 Repository나 Use-case를 참조가능한 클래스에서 Connection Handler interface를 구현하여
    Endpoint에 넘겨야 한다.
    // Connection의 생성과 파괴에 연관된 Repository들이 onConnected와 onDisconnected에서 붙여진다.
    // 예) GameChannelPool, Session 관련 Repository


# Presentation/

1. Endpoint와 Connection 모두 생성자로 ConnectionHandler&를 받고 멤버변수로 보관한다.

2. Connection은 Connection 생성 완료 직전, 다음의 행동을 한다.
    - ConnectionHandler.onConnected 호출하고 IoGameChannel 받음.
    - IoGameChannel connection에 바인딩한다.
    // IoGameChannel reference 타입을 멤버변수로 가지고 있으니, Connection에 어떻게 멤버 변수로 있을지 방법을 강구해봅시다.
    - requester의 reponse와 event에 Response 또는 Event를 SentPacket을 submit하는 Listener를 등록한다. 

3. Connection이 끊어지면 ConnectionPool에서 Connection 소멸자가 호출될 것이다. 소멸자에서 다음의 행동을 한다.
    - ConnectionHandler.onDisconnected()를 호출한다.

# 어떻게 하지?

1. Event를 Sink에 넘기고 나서 연관된 세션에서 모든 Event를 다 처리하고 나서 Event에 해당하는 데이터를 반납하는 방법. reference_count를 세긴 하는데 잘 작동하는지 모르겠음.