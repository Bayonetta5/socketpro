
from spa import *
import datetime

sidStreamSystem = BaseServiceID.sidReserved + 1210
idQueryMaxMinAvgs = tagBaseRequestID.idReservedTwo + 1
idGetMasterSlaveConnectedSessions = tagBaseRequestID.idReservedTwo + 2
idUploadEmployees = tagBaseRequestID.idReservedTwo + 3
idGetRentalDateTimes = tagBaseRequestID.idReservedTwo + 4

class CLongArray(IUSerializer):
    def __init__(self):
        self._list_ = []

    def __iter__(self):
        return self._list_.__iter__()

    def LoadFrom(self, q):
        self._list_ = []
        size = q.LoadUInt()
        while size > 0:
            self._list_.append(q.LoadLong())
            size -= 1

    def SaveTo(self, q):
        size = len(self._list_)
        q.SaveUInt(size)
        for n in self._list_:
            q.SaveLong(n)

    @property
    def list(self):
        return self._list_

class CMaxMinAvg(IUSerializer):
    def __init__(self):
        self.Max = 0.0
        self.Min = 0.0
        self.Avg = 0.0

    def LoadFrom(self, q):
        self.Max = q.LoadDouble()
        self.Min = q.LoadDouble()
        self.Avg = q.LoadDouble()

    def SaveTo(self, q):
        q.SaveDouble(self.Max).SaveDouble(self.Min).SaveDouble(self.Avg)

class CRentalDateTimes(IUSerializer):
    def __init__(self, rentalId=0):
        self.rental_id = rentalId
        self.Rental = datetime.datetime(1900, 1, 1)
        self.Return = datetime.datetime(1900, 1, 1)
        self.LastUpdate = datetime.datetime(1900, 1, 1)

    def LoadFrom(self, q):
        self.rental_id = q.LoadLong()
        self.Rental = q.LoadDate()
        self.Return = q.LoadDate()
        self.LastUpdate = q.LoadDate()

    def SaveTo(self, q):
        q.SaveLong(self.rental_id).SaveDate(self.Rental).SaveDate(self.Return).SaveDate(self.LastUpdate)