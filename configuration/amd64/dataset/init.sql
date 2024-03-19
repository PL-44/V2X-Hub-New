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
    numCars int NOT NULL,
    postedSpeed bigint NOT NULL,
    avgSpeed bigint NOT NULL,
    throughput bigint NOT NULL,
    PRIMARY KEY(roadSegmentId, timePeriod),
    FOREIGN KEY(roadSegmentId) references RoadSegments(id),
    FOREIGN KEY(timePeriod) references RoadTime(timePeriod)
);

INSERT INTO RoadSegments(id, speedLimitKMH) 
VALUES
(1, 50);

INSERT INTO RoadTime(timePeriod) 
VALUES
('2024-03-05 14:34:00'),
('2024-03-05 14:34:05'),
('2024-03-05 14:34:10');

INSERT INTO During(roadSegmentId, timePeriod, numCars, postedSpeed, avgSpeed, throughput)
VALUES
(1, '2024-03-05 14:34:00', 5, 30, 27, 5),
(1, '2024-03-05 14:34:05', 6, 30, 28, 7),
(1, '2024-03-05 14:34:10', 6, 30, 29, 6);