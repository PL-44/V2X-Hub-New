CREATE TABLE RoadSegments
(
    id int PRIMARY KEY,
    speedLimitKMH int NOT NULL
);

CREATE TABLE RoadTime
(
    timePeriod timestamp PRIMARY KEY
);

CREATE TABLE During
(
    roadSegmentId int,
    timePeriod timestamp,
    avgSpeed int NOT NULL,
    throughput int NOT NULL,
    PRIMARY KEY(roadSegmentId, timePeriod),
    FOREIGN KEY(roadSegmentId) references RoadSegments(id),
    FOREIGN KEY(timePeriod) references RoadTime(timePeriod)
);

INSERT INTO RoadSegments(id, speedLimitKMH) 
VALUES
(1, 50);

INSERT INTO RoadTime(timePeriod) 
VALUES
('2024-03-05 14:34:00');

INSERT INTO During(roadSegmentId, timePeriod, avgSpeed, throughput)
VALUES
(1, '2024-03-05 14:34:00', 27, 5);